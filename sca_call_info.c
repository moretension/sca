#include "sca_common.h"

#include <assert.h>

#include "sca.h"
#include "sca_appearance.h"
#include "sca_call_info.h"
#include "sca_dialog.h"
#include "sca_event.h"
#include "sca_notify.h"
#include "sca_subscribe.h"
#include "sca_util.h"

const str	SCA_CALL_INFO_HEADER_STR = STR_STATIC_INIT( "Call-Info: " );
const str	SCA_CALL_INFO_HEADER_NAME = STR_STATIC_INIT( "Call-Info" );

    static int
sca_call_info_domain_from_uri( str *uri, str *domain )
{
    assert( !SCA_STR_EMPTY( uri ));
    assert( domain != NULL );

    domain->s = memchr( uri->s, '@', uri->len );
    if ( domain->s == NULL ) {
	/* may be a sip:domain URI */
	domain->s = memchr( uri->s, ':', uri->len );
	if ( domain->s == NULL ) {
	    LM_ERR( "Bad URI %.*s", STR_FMT( uri ));
	    return( -1 );
	}
    }
    domain->s++;

    domain->len = ( uri->s + uri->len ) - domain->s;
    /* XXX handle :port in URI? */

    return( domain->len );
}

    static int
sca_call_info_header_length( int given_length )
{
    assert( given_length >= 0 );

    /*
     * given_length is assumed to contain length of index string,
     * state string, and uri string, if any
     */

     /* "<sip:DOMAIN>;appearance-index=N;appearance-state=STATE" */
    given_length += strlen( "<sip:>=;=;" );
    given_length += SCA_APPEARANCE_INDEX_STR.len;
    given_length += SCA_APPEARANCE_STATE_STR.len;

    return( given_length );
}

    static int
sca_call_info_header_length_for_appearance( sca_appearance *appearance,
	str *aor )
{
    int		len = 0;
    str		domain = STR_NULL;
    str		state_str = STR_NULL;

    assert( aor != NULL );

    /* get length of stringified index, since conversion's destructive */
    (void)int2str( appearance->index, &len );

    sca_appearance_state_to_str( appearance->state, &state_str );
    len += state_str.len;

    if ( !SCA_STR_EMPTY( &appearance->uri )) {
	/* +1 for ';', +1 for '=' between param name and value, +2 for quotes */
	len += SCA_APPEARANCE_URI_STR.len + 1 + 1 + 2;
	len += appearance->uri.len;
    }

    if ( sca_call_info_domain_from_uri( aor, &domain ) < 0 ) {
	return( -1 );
    }
    len += domain.len;
    
    len += sca_call_info_header_length( domain.len );

    return( len );
}

    static int
sca_call_info_header_append_appearances( sca_mod *scam, sca_subscription *sub, 
	char *hdrbuf, int maxlen )
{
    sca_appearance_list	*app_list;
    sca_appearance	*app;
    sca_hash_slot	*slot;
    str			domain;
    str			state_str;
    int			slot_idx;
    int			len = -1;
    int			usedlen = -1;

    slot_idx = sca_hash_table_index_for_key( scam->appearances,
					     &sub->target_aor );
    slot = sca_hash_table_slot_for_index( scam->appearances, slot_idx );

    sca_hash_table_lock_index( scam->appearances, slot_idx );

    app_list = sca_hash_table_slot_kv_find_unsafe( slot, &sub->target_aor );
    if ( app_list == NULL ) {
	len = 0;
	goto done;
    }

    usedlen = 0;
    for ( app = app_list->appearances; app != NULL; app = app->next ) {
	len = sca_call_info_header_length_for_appearance( app,
							  &sub->target_aor );
	if ( len < 0 ) {
	    goto done;
	}

	if (( maxlen - len ) < 0 ) {
	    LM_ERR( "Call-Info header for AoR %.*s is too long",
		    STR_FMT( &sub->target_aor ));
	    len = -1;
	    goto done;
	}

	memcpy( hdrbuf, "<sip:", strlen( "<sip:" ));
	len = strlen( "<sip:" );

	if ( sca_call_info_domain_from_uri( &sub->target_aor, &domain ) < 0 ) {
	    return( -1 );
	}

	memcpy( hdrbuf + len, domain.s, domain.len );
	len += domain.len;

	sca_appearance_state_to_str( app->state, &state_str );

	/* state_str.s is a nul-terminated string literal */
	len += snprintf( hdrbuf + len, maxlen - len,
		  ">;appearance-index=%d;appearance-state=%s",
		  app->index, state_str.s );

	if ( !SCA_STR_EMPTY( &app->uri )) {
	    hdrbuf[ len ] = ';';
	    len += 1;

	    memcpy( hdrbuf + len, SCA_APPEARANCE_URI_STR.s,
				  SCA_APPEARANCE_URI_STR.len );
	    len += SCA_APPEARANCE_URI_STR.len;

	    hdrbuf[ len ] = '=';
	    len += 1;

	    hdrbuf[ len ] = '"';
	    len += 1;

	    memcpy( hdrbuf + len, app->uri.s, app->uri.len );
	    len += app->uri.len;

	    hdrbuf[ len ] = '"';
	    len += 1;
	}

	if ( app->next ) {
	    memcpy( hdrbuf + len, ",", 1 );
	    len++;
	}

	maxlen -= len;
	hdrbuf += len;

	usedlen += len;
    }

done:
    sca_hash_table_unlock_index( scam->appearances, slot_idx );

    return( usedlen );
}

    static int
sca_call_info_header_length_for_idle_appearance( sca_mod *scam )
{
    int		given_length = 0;

    /* appearance-index=* */
    given_length += strlen( "*" );

    /* appearance-state=idle */
    given_length += SCA_APPEARANCE_STATE_STR_IDLE.len;

    given_length += scam->cfg->domain->len;

    return( sca_call_info_header_length( given_length ));
}

    static int
sca_call_info_build_idle_value( sca_mod *scam, char *hdrbuf, int maxlen )
{
    int			len;

    if ( sca_call_info_header_length_for_idle_appearance( scam ) >= maxlen ) {
	LM_ERR( "Failed to add idle appearances: Call-Info header too long" );
	return( -1 );
    }

#ifdef notdef
    strcpy( hdrbuf, "<sip:" );
    len = strlen( "<sip:" );

    memcpy( hdrbuf + len, scam->cfg->domain->s, scam->cfg->domain->len );
    len += scam->cfg->domain->len;
#endif /* notdef */

    /* the SCA_APPEARANCE_ strs' s member are literal C strings */
    len = snprintf( hdrbuf, maxlen,
		"<sip:%s>;%s=*;%s=%s%s",
		scam->cfg->domain->s,
		SCA_APPEARANCE_INDEX_STR.s,
		SCA_APPEARANCE_STATE_STR.s,
		SCA_APPEARANCE_STATE_STR_IDLE.s, CRLF );

    return( len );
}

    int
sca_call_info_build_header( sca_mod *scam, sca_subscription *sub,
	char *hdrbuf, int maxlen )
{
    /* we send one Call-Info header, appearances separated by commas */
    int			len;
    int			usedlen = SCA_CALL_INFO_HEADER_STR.len;

    /* begin with "Call-Info: " */
    memcpy( hdrbuf, SCA_CALL_INFO_HEADER_STR.s, SCA_CALL_INFO_HEADER_STR.len );

    len = sca_call_info_header_append_appearances( scam, sub,
			hdrbuf + usedlen, maxlen - usedlen );
    usedlen += len;
    if ( usedlen > SCA_CALL_INFO_HEADER_STR.len ) {
	/* we added an indexed appearance, append a comma */
	memcpy( hdrbuf + usedlen, ",", 1 );
	usedlen++;
    }

    /* line-seize NOTIFYs will contain only the seized appearance index */
    if ( sub->event != SCA_EVENT_TYPE_LINE_SEIZE ) {
	/* if not all appearances in use, add *-index idle */
	len = sca_call_info_build_idle_value( scam,
		    hdrbuf + usedlen, maxlen - usedlen );
	if ( len < 0 || len + usedlen >= maxlen ) {
	    LM_ERR( "Cannot build idle Call-Info value: buffer too small" );
	    return( -1 );
	}
	usedlen += len;
    }

    return( usedlen );
}

    int
sca_call_info_append_header_for_appearance_index( sca_subscription *sub,
	int appearance_index, char *hdrbuf, int maxlen )
{
    str		domain = STR_NULL;
    char	*app_index_p = NULL;
    int		len = 0, idx_len;

    memcpy( hdrbuf, SCA_CALL_INFO_HEADER_STR.s, SCA_CALL_INFO_HEADER_STR.len );
    len += SCA_CALL_INFO_HEADER_STR.len;
    if ( len >= maxlen ) {
	goto error;
    }

    memcpy( hdrbuf + len, "<sip:", strlen( "<sip:" ));
    len += strlen( "<sip:" );
    if ( len >= maxlen ) {
	goto error;
    }

    sca_call_info_domain_from_uri( &sub->target_aor, &domain );
    memcpy( hdrbuf + len, domain.s, domain.len );
    len += domain.len;
    if ( len >= maxlen ) {
	goto error;
    }

    memcpy( hdrbuf + len, ">;appearance-index=",
		strlen( ">;appearance-index=" ));
    len += strlen( ">;appearance-index=" );
    if ( len >= maxlen ) {
	goto error;
    }

    app_index_p = int2str( appearance_index, &idx_len );	
    memcpy( hdrbuf + len, app_index_p, idx_len );
    len += idx_len;
    if ( len >= maxlen ) {
	goto error;
    }

    memcpy( hdrbuf + len, CRLF, CRLF_LEN );
    len += CRLF_LEN;
    if ( len >= maxlen ) {
	goto error;
    }

    return( len );

error:
    LM_ERR( "Failed to append Call-Info header for %.*s appearance index %d",
		STR_FMT( &sub->subscriber ), appearance_index );
    return( -1 );
}

    hdr_field_t *
sca_call_info_header_find( hdr_field_t *msg_hdrs )
{
    hdr_field_t		*hdr = NULL;

    for ( hdr = msg_hdrs; hdr != NULL; hdr = hdr->next ) {
	if ( hdr->type == HDR_OTHER_T &&
		hdr->name.len == SCA_CALL_INFO_HEADER_NAME.len ) {
	    if ( strncasecmp( hdr->name.s, SCA_CALL_INFO_HEADER_NAME.s,
			     SCA_CALL_INFO_HEADER_NAME.len ) == 0) {
		break;
	    }
	}
    }

    return( hdr );
}

    int
sca_call_info_body_parse( str *hdr_body, sca_call_info *call_info )
{
    str		s = STR_NULL;
    char	*p;
    char	*semi;
    int		len;

    assert( call_info != NULL );

    if ( SCA_STR_EMPTY( hdr_body )) {
	LM_ERR( "Call-Info header body is empty" );
	return( -1 );
    }

    call_info->sca_uri.s = NULL;
    call_info->sca_uri.len = 0;
    call_info->index = -1;
    call_info->state = SCA_APPEARANCE_STATE_UNKNOWN;
    call_info->uri.s = NULL;
    call_info->uri.len = 0;

    p = hdr_body->s;
    if ( memcmp( p, "<sip:", strlen( "<sip:" )) != 0 ) {
	LM_ERR( "Bad Call-Info header body: must begin with \"<sip:\"" );
	return( -1 );
    }
    /* +5 == strlen( "<sip:" ) */
    semi = memchr( p + 5, ';', hdr_body->len );
    if ( semi == NULL ) {
	LM_ERR( "Bad Call-Info header body: missing ';' between uri and "
		"%.*s", STR_FMT( &SCA_APPEARANCE_INDEX_STR ));
	return( -1 );
    }
    if ( *(semi - 1) != '>' ) {
	LM_ERR( "Bad Call-Info header body: SCA URI missing '>' terminator" );
	return( -1 );
    }

    call_info->sca_uri.s = p;
    call_info->sca_uri.len = semi - p;

    p = semi;
    p++;
    if ( memcmp( p, SCA_APPEARANCE_INDEX_STR.s,
			    SCA_APPEARANCE_INDEX_STR.len ) != 0 ) {
	LM_ERR( "Bad Call-Info header body: does not begin with %.*s",
		STR_FMT( &SCA_APPEARANCE_INDEX_STR ));
	return( -1 );
    }

    p += SCA_APPEARANCE_INDEX_STR.len;
    if ( *p != '=' ) {
	LM_ERR( "Bad Call-Info header body: missing '=' after %.*s",
		STR_FMT( &SCA_APPEARANCE_INDEX_STR ));
	return( -1 );
    }

    p++;
    len = ( hdr_body->s + hdr_body->len ) - p;
    semi = memchr( p, ';', len );
    if ( semi != NULL ) {
	len = semi - p;
    }
    s.s = p;
    s.len = len;

    if ( str2int( &s, (unsigned int *)&call_info->index ) != 0 ) {
	LM_ERR( "Bad Call-Info header: failed to convert %.*s %.*s to an "
		"integer", STR_FMT( &SCA_APPEARANCE_INDEX_STR ), STR_FMT( &s ));
	return( -1 );
    }

    if ( semi == NULL ) {
	/* Call-Info header only contained an appearance-index */
	goto done;
    }

    /* advance appearance-index value + semi-colon */
    p += ( len + 1 );
    if ( memcmp( p, SCA_APPEARANCE_STATE_STR.s,
		    SCA_APPEARANCE_STATE_STR.len ) != 0 ) {
	LM_ERR( "Bad Call-Info header: missing %.*s",
		STR_FMT( &SCA_APPEARANCE_STATE_STR ));
	return( -1 );
    }

    p += SCA_APPEARANCE_STATE_STR.len;
    if ( *p != '=' ) {
	LM_ERR( "Bad Call-Info header body: missing '=' after %.*s",
		STR_FMT( &SCA_APPEARANCE_STATE_STR ));
	return( -1 );
	
    }

    p++;
    len = ( hdr_body->s + hdr_body->len ) - p;
    semi = memchr( p, ';', len );
    if ( semi != NULL ) {
	len = semi - p;
    }
    s.s = p;
    s.len = len;

    call_info->state = sca_appearance_state_from_str( &s );
    if ( call_info->state == SCA_APPEARANCE_STATE_UNKNOWN ) {
	LM_ERR( "Bad Call-Info header: unrecognized state \"%.*s\"",
		STR_FMT( &s ));
	return( -1 );
    }

    if ( semi == NULL ) {
	/* Call-Info header only had appearance-index & appearance-state */
	goto done;
    }

    /* advance length of state + semi-colon */
    p += ( len + 1 );
    if ( memcmp( p, SCA_APPEARANCE_URI_STR.s,
		 SCA_APPEARANCE_URI_STR.len ) != 0 ) {
	LM_ERR( "Bad Call-Info header: missing %.*s",
		STR_FMT( &SCA_APPEARANCE_URI_STR ));
	return( -1 );
    }

    p += SCA_APPEARANCE_URI_STR.len;
    if ( *p != '=' ) {
	LM_ERR( "Bad Call-Info header: missing '=' after %.*s",
		STR_FMT( &SCA_APPEARANCE_URI_STR ));
	return( -1 );
    }

    p++;
    call_info->uri.s = p;
    call_info->uri.len = ( hdr_body->s + hdr_body->len ) - p;

    if ( SCA_STR_EMPTY( &call_info->uri )) {
	LM_ERR( "Bad Call-Info header: empty %.*s",
		STR_FMT( &SCA_APPEARANCE_URI_STR ));
	return( -1 );
    }

done:
    return( 0 );
}

/*
 * return codes:
 *	-1: error, failed to remove call-info header.
 *	 0: no call-info headers found.
 *     >=1: removed 1 or more call-info headers.
 */
    static int
sca_call_info_header_remove( sip_msg_t *msg )
{
    hdr_field_t		*hdr;
    struct lump		*ci_hdr_lump;
    int			rc = 0;

    /* all headers must be parsed before using del_lump */
    if ( parse_headers( msg, HDR_EOH_F, 0 ) < 0 ) {
	LM_ERR( "Failed to parse_headers" );
	return( -1 );
    }

#ifdef notdef
    for ( hdr = sca_call_info_header_find( msg->headers ); hdr != NULL;
		hdr = sca_call_info_header_find( hdr->next )) {
#endif /* notdef */
    for ( hdr = msg->headers; hdr; hdr = hdr->next ) {
	if ( hdr->name.len != SCA_CALL_INFO_HEADER_NAME.len ) {
	    continue;
	}
	if ( memcmp( hdr->name.s, SCA_CALL_INFO_HEADER_NAME.s,
				hdr->name.len ) != 0 ) {
	    continue;
	}

	/* del_lump takes packet, offset, lump length, & hdr type */
	ci_hdr_lump = del_lump( msg, hdr->name.s - msg->buf,
				hdr->len, HDR_OTHER_T );
	if ( ci_hdr_lump == NULL ) {
	    LM_ERR( "Failed to del_lump Call-Info header" );
	    rc = -1;
	    break;
	}

	rc++;
    }

    return( rc );
}

    //static int
    int
sca_call_info_seize_held_call( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor, str *to_aor,
	str *contact_uri )
{
    sca_appearance	*app;
    struct lump		*anchor;
    str			callee_aor = STR_NULL;
    str			replaces_hdr = STR_NULL;
    str			prev_callid = STR_NULL;
    str			prev_totag = STR_NULL;
    char		tagbuf[ 1024 ];
    char		callee_buf[ 1024 ];
    int			slot_idx = -1;
    int			rc = -1;

    slot_idx = sca_hash_table_index_for_key( sca->appearances, from_aor );
    sca_hash_table_lock_index( sca->appearances, slot_idx );

    app = sca_appearance_for_index_unsafe( sca, from_aor, call_info->index,
					   slot_idx );
    if ( app == NULL ) {
	LM_ERR( "sca_call_info_seize_held_call: no active appearances for "
		"%.*s", STR_FMT( from_aor ));
	goto done;
    }

LM_INFO( "ADMORTEN DEBUG: seizing %.*s appearance-index %d, callee %.*s",
	STR_FMT( from_aor ), app->index, STR_FMT( &app->callee ));

    /* rewrite the RURI to use the callee in this SCA dialog */
    if ( msg->new_uri.s ) {
	/*
	 * someone already rewrote the URI. shouldn't happen, but we have
	 * to watch for it. log our overwriting of it.
	 */

	LM_INFO( "SCA caller retrieving held call, but RURI was already "
		 "rewritten as %.*s. Overwriting with %.*s.",
		 STR_FMT( &msg->new_uri ), STR_FMT( &app->callee ));

	pkg_free( msg->new_uri.s );
	msg->new_uri.s = NULL;
	msg->new_uri.len = 0;
    }

    /* msg->new_uri.s is free'd when transaction is torn down */
    msg->new_uri.s = (char *)pkg_malloc( app->callee.len );
    if ( msg->new_uri.s == NULL ) {
	LM_ERR( "sca_call_info_seize_held_call: pkg_malloc new RURI %.*s "
		"failed", STR_FMT( &app->callee ));
	goto done;
    }
    SCA_STR_COPY( &msg->new_uri, &app->callee );

    {
	int		idx; 

	for ( idx = 0; get_sip_branch( idx ) != NULL; idx++ )
	    ;

LM_INFO( "ADMORTEN DEBUG: %d sip branches", idx + 1 );

	for ( ; idx >= 0; idx-- ) {
LM_INFO( "ADMORTEN DEBUG: dropping branch %d", idx );
	    drop_sip_branch( idx );
	}
    }

    /* must reset to avoid using cached parsed RURI */
    msg->parsed_uri_ok = 0;
    ruri_mark_new();

    /* store the previous dialog's tags for lookup of the callee */
    prev_callid.s = tagbuf;
    SCA_STR_COPY( &prev_callid, &app->dialog.call_id );

    prev_totag.s = tagbuf + prev_callid.len;
    SCA_STR_COPY( &prev_totag, &app->dialog.to_tag );

    /* pkg_malloc's replaces_hdr.s, which is free'd if added as lump */
    if ( sca_dialog_create_replaces_header( &app->dialog,
					    &replaces_hdr ) < 0 ) {
	LM_ERR( "sca_call_info_seize_held_call: failed to create Replaces "
		"header for %.*s from dialog %.*s",
		STR_FMT( from_aor ), STR_FMT( &app->dialog.id ));
	goto done;
    }

    /* store the callee's username for lookup of the callee by AoR */
    callee_aor.s = callee_buf;
    if ( sca_uri_build_aor( &callee_aor, sizeof( callee_buf ), &app->callee,
			    from_aor ) < 0 ) {
	LM_ERR( "sca_call_info_seize_held_call: failed to create To AoR "
		"from %.*s and %.*s", STR_FMT( &app->callee ),
		STR_FMT( from_aor ));
	pkg_free( replaces_hdr.s );
	goto done;
    }

    /* all headers must be parsed before using lump functions */
    if ( parse_headers( msg, HDR_EOH_F, 0 ) < 0 ) {
	LM_ERR( "Failed to parse_headers" );
	goto done;
    }

    anchor = anchor_lump( msg, msg->eoh - msg->buf, 0, HDR_OTHER_T );
    if ( anchor == NULL ) {
	LM_ERR( "Failed to anchor lump" );
	goto done;
    }

    /* append the Replaces header before the sdp body */
    if ( insert_new_lump_before( anchor, replaces_hdr.s, 
	    replaces_hdr.len, HDR_OTHER_T ) == NULL ) {
	LM_ERR( "Failed to add Replaces header %.*s", STR_FMT( &replaces_hdr ));
	pkg_free( replaces_hdr.s );
	goto done;
    }

    /*
     * RFC 3891 (Replaces header) suggests, but does not require, that the
     * UAS establish the dialog with the UAC replacing the existing dialog
     * before sending the BYE to the original UAC. Polycom handsets appear
     * to send the BYE to the original UAC first, so we save the pending
     * owner here. if the 200 OK arrives first, we update the owner and
     * dialog there. otherwise, we catch this in the 200 OK to the BYE
     * sent by the line being replaced.
     *
     * if the reINVITE to seize the held line fails for some reason,
     * we restore the original owner and dialog.
     */

    if ( sca_appearance_update_owner_unsafe( app, contact_uri ) < 0 ) {
	LM_ERR( "sca_call_info_seize_held_call: failed to update owner" );
	pkg_free( replaces_hdr.s );
	goto done;
    }

    if ( sca_appearance_update_dialog_unsafe( app, &msg->callid->body,
				    &from->tag_value, &to->tag_value ) < 0 ) {
	LM_ERR( "sca_call_info_seize_held_call: failed to update dialog" );
	goto done;
    }

    app->flags |= SCA_APPEARANCE_FLAG_OWNER_PENDING;
    app->state = SCA_APPEARANCE_STATE_ACTIVE;

    sca_hash_table_unlock_index( sca->appearances, slot_idx );
    slot_idx = -1;

    if ( !SCA_STR_EMPTY( &callee_aor )) {
	if ( sca_uri_lock_if_shared_appearance( sca, &callee_aor, &slot_idx )) {
	    app = sca_appearance_for_tags_unsafe( sca, &callee_aor,
			&prev_callid, &prev_totag, NULL, slot_idx );
	    if ( app == NULL ) {
		LM_ERR( "sca_call_info_seize_held_call: failed to find "
			"appearance of %.*s with dialog %.*s;%.*s",
			STR_FMT( &callee_aor ), STR_FMT( &prev_callid ),
			STR_FMT( &prev_totag ));
		goto done;
	    }

	    app->flags |= SCA_APPEARANCE_FLAG_CALLEE_PENDING;

	    if ( sca_appearance_update_callee_unsafe( app, contact_uri ) < 0 ) {
		LM_ERR( "sca_call_info_seize_held_call: failed to update callee" );
		goto done;
	    }
	    if ( sca_appearance_update_dialog_unsafe( app, &msg->callid->body,
					&to->tag_value, &from->tag_value ) < 0 ) {
		LM_ERR( "sca_call_info_seize_held_call: failed to update dialog" );
		goto done;
	    }
	}
    }

    rc = 1;

done:
    if ( slot_idx >= 0 ) {
	sca_hash_table_unlock_index( sca->appearances, slot_idx );
    }

    return( rc );
}

    static int
sca_call_info_uri_update( str *aor, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *contact_uri,
	str *call_id )
{
    sca_appearance	*app;
    sca_dialog		dialog;
    str			state_str;
    str			*from_tag = &from->tag_value;
    str			*to_tag = &to->tag_value;
    char		dlg_buf[ 1024 ];
    int			slot_idx = -1;
    int			rc = -1;

    assert( aor != NULL );
    assert( call_info != NULL );

LM_INFO( "ADMORTEN DEBUG: sca_call_info_uri update for %.*s: "
	 "From: <%.*s> To: <%.*s> Contact: <%.*s> Call-ID: %.*s "
	 "Call-Info: appearance-index=%d", STR_FMT( aor ),
	 STR_FMT( &from->uri ), STR_FMT( &to->uri ), STR_FMT( contact_uri ),
	 STR_FMT( call_id ), call_info->index );

    if ( !sca_uri_is_shared_appearance( sca, aor )) {
	return( 0 );
    }

    dialog.id.s = dlg_buf;
    if ( sca_dialog_build_from_tags( &dialog, sizeof( dlg_buf ), call_id,
				to_tag, from_tag ) < 0 ) {
	LM_ERR( "sca_call_info_uri_update: Failed to build dialog from tags" );
	return( -1 );
    }

    slot_idx = sca_hash_table_index_for_key( sca->appearances, aor );
    sca_hash_table_lock_index( sca->appearances, slot_idx );

    app = sca_appearance_for_index_unsafe( sca, aor, call_info->index,
						slot_idx );
    if ( app != NULL ) {
LM_INFO( "ADMORTEN DEBUG: found appearance for %.*s", STR_FMT( aor ));
LM_INFO( "ADMORTEN DEBUG: setting owner to %.*s", STR_FMT( contact_uri ));
	if ( sca_appearance_update_unsafe( app, call_info->state,
		NULL, NULL, &dialog, contact_uri, NULL ) < 0 ) {
	    sca_appearance_state_to_str( call_info->state, &state_str );
	    LM_ERR( "sca_call_info_uri_update: failed to update appearance "
		    "%.*s appearance-index %d with dialog id %.*s to "
		    "state %.*s", STR_FMT( &app->owner ), app->index,
		    STR_FMT( &app->dialog.id ), STR_FMT( &state_str ));
	    goto done;
	}

	rc = 1;
    } else {
LM_INFO( "ADMORTEN DEBUG: no appearance for %.*s, seizing next", STR_FMT( aor ));
	app = sca_appearance_seize_next_available_unsafe( sca, aor,
				contact_uri, slot_idx );
	if ( app == NULL ) {
	    LM_ERR( "sca_call_info_uri_update: failed to seize index %d "
		    "for %.*s", call_info->index, STR_FMT( contact_uri ));
	    goto done;
	}

LM_INFO( "ADMORTEN DEBUG: seized %d for %.*s: From: <%.*s> To: <%.*s> "
	 "Call-ID: <%.*s> Dialog: <%.*s>" , app->index,
	 STR_FMT( &app->owner ), STR_FMT( &from->uri ), STR_FMT( &to->uri ),
	 STR_FMT( call_id ), STR_FMT( &app->dialog.id ));

	if ( sca_appearance_update_unsafe( app, SCA_APPEARANCE_STATE_ACTIVE,
		&from->display, &from->uri, &dialog, contact_uri,
		&from->uri ) < 0 ) {
	    sca_appearance_state_to_str( call_info->state, &state_str );
	    LM_ERR( "sca_call_info_uri_update: failed to update appearance "
		    "%.*s appearance-index %d with dialog id %.*s to "
		    "state %.*s", STR_FMT( &app->owner ), app->index,
		    STR_FMT( &app->dialog.id ), STR_FMT( &state_str ));
	    goto done;
	}

	rc = 1;
    }

done:
    if ( slot_idx >= 0 ) {
	sca_hash_table_unlock_index( sca->appearances, slot_idx );
    }

    return( rc );
}

    static int
sca_call_info_is_line_seize_reinvite( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor, str *to_aor )
{
    str			*ruri;
    str			ruri_aor;
    int			state;

    /*
     * a handset in an SCA group is attempting to seize a held line if:
     *		the RURI, From URI and To URI are identical;
     *		the above are SCA AoRs;
     *		there is no to-tag;
     *		a Call-Info header is present
     */

    if ( call_info == NULL ) {
	return( 0 );
    }
    if ( !SCA_STR_EMPTY( &to->tag_value )) {
	return( 0 );
    }

    ruri = GET_RURI( msg );
    if ( sca_uri_extract_aor( ruri, &ruri_aor ) < 0 ) {
	LM_ERR( "sca_call_info_is_line_seize_reinvite: failed to extract "
		"AoR from RURI %.*s", STR_FMT( ruri ));
	return( 0 );
    }

    if ( !SCA_STR_EQ( from_aor, to_aor )) {
LM_INFO( "ADMORTEN DEBUG: %.*s != %.*s != %.*s",
	    STR_FMT( &ruri_aor ), STR_FMT( from_aor ), STR_FMT( to_aor ));
	return( 0 );
    }

    state = sca_appearance_state_for_index( sca, from_aor, call_info->index );
    if ( state != SCA_APPEARANCE_STATE_HELD ) {
	LM_DBG( "sca_call_info_is_line_seize_reinvite: new INVITE to "
		"%.*s from %.*s appearance-index %d (not seizing held line)",
		STR_FMT( to_aor ), STR_FMT( from_aor ), call_info->index );
	return( 0 );
    }

    return( 1 );
}

    int
sca_call_info_invite_request_handler( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor, str *to_aor,
	str *contact_uri )
{
    sca_dialog		dialog;
    char		dlg_buf[ 1024 ];
    str			state_str = STR_NULL;
    int			state = SCA_APPEARANCE_STATE_UNKNOWN;
    int			rc = -1;

    /*
     * if we get here, one of the legs is an SCA endpoint. we want to know
     * when the e2e ACK comes in so we can notify other members of the group.
     */
    if ( sca->tm_api->register_tmcb( msg, NULL, TMCB_E2EACK_IN,
				    sca_call_info_ack_cb, NULL, NULL ) < 0 ) {
	LM_ERR( "sca_call_info_invite_request_handler: failed to register "
		"callback for INVITE %.*s ACK", STR_FMT( from_aor ));
	goto done;
    }

    if ( call_info == NULL ) {
	/* caller isn't SCA, no more to do. update callee in reply handler. */
	rc = 1;
	goto done;
    }

    if ( sca_call_is_held( msg )) {
	state = SCA_APPEARANCE_STATE_HELD;
    } else if ( !SCA_STR_EMPTY( &to->tag_value )) {
	/* this is a reINVITE from an SCA line that put the call on hold */
	state = SCA_APPEARANCE_STATE_ACTIVE;
    } else if ( sca_call_info_is_line_seize_reinvite( msg, call_info,
					    from, to, from_aor, to_aor )) {
	rc = sca_call_info_seize_held_call( msg, call_info, from, to,
					   from_aor, to_aor, contact_uri );
    }
    /* otherwise, this is an initial INVITE */

    sca_appearance_state_to_str( state, &state_str );
    LM_INFO( "ADMORTEN: updating %.*s appearance-index %d to %.*s, "
	     "dialog: callid: %.*s, from-tag: %.*s",
		STR_FMT( from_aor ), call_info->index,
		STR_FMT( &state_str ),
		STR_FMT( &msg->callid->body ),
		STR_FMT( &from->tag_value ));

    dialog.id.s = dlg_buf;
    if ( sca_dialog_build_from_tags( &dialog, sizeof( dlg_buf ),
	    &msg->callid->body, &from->tag_value, &to->tag_value ) < 0 ) {
	LM_ERR( "Failed to build dialog from tags" );
	return( -1 );
    }

    if ( sca_appearance_update_index( sca, from_aor, call_info->index,
		state, NULL, NULL, &dialog ) != SCA_APPEARANCE_OK ) {
	LM_ERR( "Failed to update %.*s appearance-index %d to %.*s",
		STR_FMT( from_aor ), call_info->index,
		STR_FMT( &state_str ));
    }

    if ( sca_notify_call_info_subscribers( sca, from_aor ) < 0 ) {
	LM_ERR( "Failed to call-info NOTIFY %.*s subscribers on INVITE",
		STR_FMT( from_aor ));
	goto done;
    }

    rc = 1;

done:
    return( rc );
}

    int
sca_call_info_invite_reply_18x_handler( sip_msg_t *msg,
	sca_call_info *call_info, struct to_body *from, struct to_body *to,
	str *from_aor, str *to_aor, str *contact_uri )
{
    sca_appearance	*app = NULL;
    str			owner = STR_NULL;
    int			state = SCA_APPEARANCE_STATE_UNKNOWN;
    int			slot_idx = -1;
    int			rc = -1;
    int			notify = 0;

    switch ( msg->REPLY_STATUS ) {
    case 180:
	state = SCA_APPEARANCE_STATE_ALERTING;
	break;

    case 183:
	state = SCA_APPEARANCE_STATE_PROGRESSING;
	break;

    default:
	goto done;
    }

    if ( !sca_uri_lock_if_shared_appearance( sca, from_aor, &slot_idx )) {
	LM_DBG( "sca_call_info_invite_reply_18x_handler: From-AoR %.*s is "
		"not a shared appearance", STR_FMT( from_aor ));
	return( 1 );
    }

    app = sca_appearance_for_tags_unsafe( sca, from_aor, &msg->callid->body,
			  &from->tag_value, &to->tag_value, slot_idx );
    if ( app == NULL ) {
	goto done;
    }

    /* clone appearance owner for subscription termination below */
    owner.s = (char *)pkg_malloc( app->owner.len );
    if ( owner.s == NULL ) {
	LM_ERR( "sca_call_info_invite_18x_reply_handler: failed to "
		"pkg_malloc %d bytes to clone <%.*s>",
		app->owner.len, STR_FMT( &app->owner ));
	goto done;
    }
    SCA_STR_COPY( &owner, &app->owner );

    notify = ( app->state != state );
    app->state = state;
    rc = 1;

done:
    if ( slot_idx >= 0 ) {
	sca_hash_table_unlock_index( sca->appearances, slot_idx );
    }

    if ( rc > 0 && notify && owner.s != NULL ) {
	if ( sca_subscription_terminate( sca, from_aor,
		SCA_EVENT_TYPE_LINE_SEIZE, &owner,
		SCA_SUBSCRIPTION_STATE_TERMINATED_NORESOURCE,
		SCA_SUBSCRIPTION_TERMINATE_OPT_UNSUBSCRIBE ) < 0 ) {
	    LM_ERR( "sca_call_info_invite_reply_18x_handler: "
		    "failed to terminate line-seize subscription for %.*s",
		    STR_FMT( &owner ));
	    rc = -1;
	}

	if ( sca_notify_call_info_subscribers( sca, from_aor ) < 0 ) {
	    LM_ERR( "sca_call_info_invite_reply_18x_handler: "
		    "failed to NOTIFY %.*s call-info subscribers",
		    STR_FMT( from_aor ));
	    rc = -1;
	}
    }
    if ( owner.s != NULL ) {
	pkg_free( owner.s );
    }

    return( rc );
}

    static int
sca_call_info_insert_asserted_identity( sip_msg_t *msg, str *contact_uri,
	struct to_body *tf )
{
    struct lump		*anchor;
    sip_uri_t		c_uri;
    str			aor = STR_NULL;
    str			hdr = STR_NULL;
    int			len;
    int			rc = -1;

    anchor = anchor_lump( msg, msg->eoh - msg->buf, 0, HDR_OTHER_T );
    if ( anchor == NULL ) {
	LM_ERR( "Failed to anchor lump" );
	goto done;
    }
    
    if ( parse_uri( contact_uri->s, contact_uri->len, &c_uri ) < 0 ) {
	LM_ERR( "insert_asserted_identity: parse_uri %.*s failed",
		STR_FMT( contact_uri ));
	return( -1 );
    }
    if ( sca_aor_create_from_info( &aor, c_uri.type, &c_uri.user,
		&GET_TO_PURI( msg )->host, &GET_TO_PURI( msg )->port ) < 0 ) {
	LM_ERR( "insert_asserted_identity: sca_aor_create_from_info failed" );
	return( -1 );
    }

#define SCA_P_ASSERTED_IDENTITY_HDR_PREFIX	"P-Asserted-Identity: "
#define SCA_P_ASSERTED_IDENTITY_HDR_PREFIX_LEN	strlen("P-Asserted-Identity: ")

    len = SCA_P_ASSERTED_IDENTITY_HDR_PREFIX_LEN;
    len += tf->display.len;
    /* +1 for space, +1 for <, + 1 for > */
    len += 1 + 1 + aor.len + 1 + CRLF_LEN;

    hdr.s = (char *)pkg_malloc( len );
    if ( hdr.s == NULL ) {
	LM_ERR( "insert_asserted_identity: pkg_malloc %d bytes failed", len );
	goto done;
    }

    memcpy( hdr.s, SCA_P_ASSERTED_IDENTITY_HDR_PREFIX,
		    SCA_P_ASSERTED_IDENTITY_HDR_PREFIX_LEN );
    hdr.len = SCA_P_ASSERTED_IDENTITY_HDR_PREFIX_LEN;

    SCA_STR_APPEND( &hdr, &tf->display );

    *(hdr.s + hdr.len) = ' ';
    hdr.len++;

    *(hdr.s + hdr.len) = '<';
    hdr.len++;

    SCA_STR_APPEND( &hdr, &aor );

    *(hdr.s + hdr.len) = '>';
    hdr.len++;

    memcpy( hdr.s + hdr.len, CRLF, CRLF_LEN );
    hdr.len += CRLF_LEN;

    /* append the PAI header before the sdp body */
    if ( insert_new_lump_before( anchor, hdr.s, hdr.len, HDR_PAI_T ) == NULL ) {
	LM_ERR( "Failed to add PAI header %.*s", STR_FMT( &hdr ));
	goto done;
    }

    rc = 1;

done:
    if ( aor.s != NULL ) {
	pkg_free( aor.s );
    }
    if ( rc < 0 && hdr.s != NULL ) {
	pkg_free( hdr.s );
    }

    return( rc );
}

    static int
sca_call_info_invite_reply_200_handler( sip_msg_t *msg,
	sca_call_info *call_info, struct to_body *from, struct to_body *to,
	str *from_aor, str *to_aor, str *contact_uri )
{
    sca_appearance	*app;
    sca_dialog		dialog;
    sip_uri_t		c_uri;
    char		dlg_buf[ 1024 ];
    str			app_uri_aor = STR_NULL;
    str			state_str = STR_NULL;
    int			state = SCA_APPEARANCE_STATE_UNKNOWN;
    int			slot_idx = -1;
    int			rc = -1;

    if ( call_info != NULL ) {
	/* this implies To-AoR is SCA */

	rc = sca_call_info_uri_update( to_aor, call_info, from, to,
			contact_uri, &msg->callid->body );

	rc = sca_call_info_insert_asserted_identity( msg, contact_uri, to );
    }

    if ( !sca_uri_is_shared_appearance( sca, from_aor )) {
LM_INFO( "## ADMORTEN DEBUG: %.*s is not SCA", STR_FMT( from_aor ));
	goto done;
    }

    /*
     * XXX in a reply with no Call-Info header, we look for a matching
     * dialog for the From URI. if we don't find one, this isn't an SCA
     * call, and we're done processing.
     *
     * if there *is* a Call-Info header, we update the appearance state of
     * the index in the header for the To URI (appearance owned by the URI
     * in the Contact header). we still then need to check to see if the
     * From URI is an SCA line, and update state if it is.
     */

    slot_idx = sca_hash_table_index_for_key( sca->appearances, from_aor );
    sca_hash_table_lock_index( sca->appearances, slot_idx );

LM_INFO( "## ADMORTEN DEBUG: looking for %.*s appearance with dialog "
		"callid:<%.*s>, from-tag:<%.*s>, to-tag:<%.*s>",
		STR_FMT( from_aor ), STR_FMT( &msg->callid->body ),
		STR_FMT( &from->tag_value ), STR_FMT( &to->tag_value ));

    app = sca_appearance_for_tags_unsafe( sca, from_aor,
		&msg->callid->body, &from->tag_value, NULL, slot_idx );
    if ( app == NULL ) {
	/* no SCA line is involved with this call */
	LM_INFO( "## ADMORTEN DEBUG: %.*s is not an SCA line",
		    STR_FMT( from_aor ));
	rc = 1;
	goto done;
    }

    if ( app->state != SCA_APPEARANCE_STATE_HELD ) {
	state = SCA_APPEARANCE_STATE_ACTIVE;
    }
    /* if a Call-Info header is present, app-index goes to Contact */

    dialog.id.s = dlg_buf;
    if ( sca_dialog_build_from_tags( &dialog, sizeof( dlg_buf ),
	    &msg->callid->body, &from->tag_value, &to->tag_value ) < 0 ) {
	LM_ERR( "sca_call_info_invite_handler: failed to build sca_dialog "
		"from tags" );
	rc = -1;
	goto done;
    }

    if ( parse_uri( contact_uri->s, contact_uri->len, &c_uri ) < 0 ) {
	LM_ERR( "sca_call_info_invite_200_reply_handler: "
		"parse_uri <%.*s> failed", STR_FMT( contact_uri ));
	goto done;
    }
    if ( sca_aor_create_from_info( &app_uri_aor, c_uri.type, &c_uri.user,
		&GET_TO_PURI( msg )->host, &GET_TO_PURI( msg )->port ) < 0 ) {
	LM_ERR( "sca_call_info_invite_200_reply_handler: "
		"sca_aor_create_from_info %.*s and %.*s failed",
		STR_FMT( contact_uri ), STR_FMT( &to->uri ));
	goto done;
    }

    if ( sca_appearance_update_unsafe( app, state, &to->display, &app_uri_aor,
	    &dialog, NULL, contact_uri ) < 0 ) {
	sca_appearance_state_to_str( state, &state_str );
	LM_ERR( "sca_call_info_invite_handler: failed to update appearance "
		"%.*s appearance-index %d with dialog id %.*s to "
		"state %.*s", STR_FMT( &app->owner ), app->index,
		STR_FMT( &app->dialog.id ), STR_FMT( &state_str ));
	rc = -1;
	goto done;
    }

    rc = 1;

done:
    if ( slot_idx >= 0 ) {
	sca_hash_table_unlock_index( sca->appearances, slot_idx );
    }
    if ( app_uri_aor.s != NULL ) {
	pkg_free( app_uri_aor.s );
    }

    return( rc );
}

    static int
sca_call_info_invite_reply_3xx_handler( sip_msg_t *msg,
	sca_call_info *call_info, struct to_body *from, struct to_body *to,
	str *from_aor, str *to_aor, str *contact_uri )
{
    return( 1 );
}

    static int
sca_call_info_invite_reply_error_handler( sip_msg_t *msg,
	sca_call_info *call_info, struct to_body *from, struct to_body *to,
	str *from_aor, str *to_aor, str *contact_uri )
{
    /*
     * XXX will need special handling here. must distinguish among the
     * following:
     * 		failed initial INVITE
     *		failed reINVITE from caller retrieving from hold
     *		failed INVITE retrieving remote UA from SCA hold
     *
     * for a start, we just deal with the first case.
     */

    sca_appearance	*app;

    if ( msg->REPLY_STATUS == 487 ) {
	/* reply status for a CANCEL'd INVITE */
	return( 1 );
    }

    if ( sca_uri_is_shared_appearance( sca, from_aor )) {
	app = sca_appearance_unlink_by_tags( sca, from_aor,
		    &msg->callid->body, &from->tag_value, NULL );
	if ( app == NULL ) {
	    LM_ERR( "sca_call_info_invite_reply_error_handler: failed to "
		    "look up dialog for failed INVITE %.*s from %.*s",
		    STR_FMT( &to->uri ), STR_FMT( from_aor ));
	    return( -1 );
	}
	sca_appearance_free( app );

	if ( sca_notify_call_info_subscribers( sca, from_aor ) < 0 ) {
	    LM_ERR( "Failed to call-info NOTIFY %.*s subscribers on "
		    "failed INVITE", STR_FMT( from_aor ));
	    return( -1 );
	}
    }

    return( 1 );
}

    void
sca_call_info_ack_from_handler( sip_msg_t *msg, str *from_aor, str *to_aor )
{
    sca_appearance	*app;
    struct to_body	*from;
    struct to_body	*to;
    int			slot_idx = -1;

LM_INFO( "ADMORTEN DEBUG: entered sca_call_info_ack_from_handler" );

    if ( sca_get_msg_from_header( msg, &from ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to get From-header" );
	return;
    }
    if ( sca_get_msg_to_header( msg, &to ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to get To-header" );
	return;
    }

    if ( sca_uri_lock_if_shared_appearance( sca, from_aor, &slot_idx )) {
	app = sca_appearance_for_tags_unsafe( sca, from_aor,
			&msg->callid->body, &from->tag_value, NULL, slot_idx );
	if ( app == NULL ) {
	    LM_ERR( "sca_call_info_ack_cb: No appearance for %.*s matching "
		    "call-id <%.*s> and from-tag <%.*s>", STR_FMT( from_aor ),
		    STR_FMT( &msg->callid->body ), STR_FMT( &from->tag_value ));
	    goto done;
	}

	if (( app->flags & SCA_APPEARANCE_FLAG_OWNER_PENDING )) {
	    app->flags &= ~SCA_APPEARANCE_FLAG_OWNER_PENDING;

#ifdef notdef
	    /* set the update flag so the script will invoke update */
	    setflag( msg, (flag_t)sca->cfg->update_flag );
#endif /* notdef */
	}
    }

done:
    if ( slot_idx >= 0 ) {
	sca_hash_table_unlock_index( sca->appearances, slot_idx );
    }
}

/* XXX needs extract routines */
    void
sca_call_info_ack_cb( struct cell *t, int type, struct tmcb_params *params )
{
    struct to_body	*from;
    struct to_body	*to;
    str			from_aor = STR_NULL;
    str			to_aor = STR_NULL;
    int			slot_idx;

LM_INFO( "ADMORTEN DEBUG: entered sca_call_info_ack_cb" );

    if ( !(type & TMCB_E2EACK_IN)) {
	return;
    }

    if ( sca_get_msg_from_header( params->req, &from ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to get From-header" );
	return;
    }
    if ( sca_uri_extract_aor( &from->uri, &from_aor ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to extract From AoR from %.*s",
		STR_FMT( &from->uri ));
	return;
    }
LM_INFO( "ADMORTEN DEBUG: sca_call_info_ack_cb: from_aor: %.*s", STR_FMT( &from_aor));
    if ( sca_get_msg_to_header( params->req, &to ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to get To-header" );
	return;
    }
    if ( sca_uri_extract_aor( &to->uri, &to_aor ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to extract To AoR from %.*s",
		STR_FMT( &to->uri ));
	return;
    }

    sca_call_info_ack_from_handler( params->req, &from_aor, &to_aor );

    if ( !sca_uri_is_shared_appearance( sca, &to_aor )) {
	LM_DBG( "sca_call_info_ack_cb: %.*s is not a shared appearance",
		STR_FMT( &to_aor ));
	return;
    }

    if ( sca_notify_call_info_subscribers( sca, &to_aor ) < 0 ) {
	LM_ERR( "sca_call_info_ack_cb: failed to call-info "
		"NOTIFY %.*s subscribers", STR_FMT( &to_aor ));
	return;
    }

    LM_INFO( "ADMORTEN DEBUG: call-info NOTIFY to %.*s", STR_FMT( &to_aor ));
}

    static int
sca_call_info_invite_handler( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor, str *to_aor,
	str *contact_uri )
{
    int			rc = -1;

    if ( SCA_STR_EMPTY( contact_uri )) {
	LM_ERR( "sca_call_info_invite_handler: Contact header is empty. "
		"(From: %.*s To: %.*s)", STR_FMT( from_aor ),
		 STR_FMT( to_aor ));
	return( -1 );
    }

    if ( msg->first_line.type == SIP_REQUEST ) {
	rc = sca_call_info_invite_request_handler( msg, call_info, from, to,
					    from_aor, to_aor, contact_uri );	
    } else {
	/* XXX replace with dispatch table. */
	switch ( msg->REPLY_STATUS ) {
	case 100:
	    rc = 1;
	    break;

	case 180:
	case 183:
	    rc = sca_call_info_invite_reply_18x_handler( msg, call_info,
				from, to, from_aor, to_aor, contact_uri );
	    break;

	case 200:
	    rc = sca_call_info_invite_reply_200_handler( msg, call_info,
				from, to, from_aor, to_aor, contact_uri );
	    break;

	default:
	    rc = sca_call_info_invite_reply_error_handler( msg, call_info,
				from, to, from_aor, to_aor, contact_uri );
	    break;
	}
    }

    return( rc );
}

    static int
sca_call_info_bye_handler( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor, str *to_aor,
	str *contact_uri )
{
    sca_appearance	*app, *unl_app;
    int			slot_idx = -1;
    int			rc = -1;

    if ( msg->first_line.type == SIP_REQUEST ) {
	if ( call_info != NULL ) {
	    slot_idx = sca_uri_lock_shared_appearance( sca, from_aor );
	    if ( slot_idx < 0 ) {
		LM_ERR( "sca_call_info_bye_handler: failed to acquire "
			"lock for %.*s, appearance-index %.d",
			STR_FMT( from_aor ), call_info->index );
		goto done;
	    }

	    app = sca_appearance_for_index_unsafe( sca, from_aor,
			call_info->index, slot_idx );
	    if ( app == NULL ) {
		LM_ERR( "sca_call_info_bye_handler: %.*s "
			"appearance-index %d is not active",
			STR_FMT( from_aor ), call_info->index );
		goto done;
	    }

	    if (( app->flags & SCA_APPEARANCE_FLAG_CALLEE_PENDING )) {
		app->flags &= ~SCA_APPEARANCE_FLAG_CALLEE_PENDING;
	    } else {
		/* XXX yes, duplicated below, too */
		unl_app = sca_appearance_list_unlink_index(
					app->appearance_list, app->index );
		if ( unl_app == NULL || unl_app != app ) {
		    LM_ERR( "sca_call_info_bye_handler: failed to unlink "
			    "%.*s appearance-index %d, owner %.*s",
			    STR_FMT( &app->owner ), app->index,
			    STR_FMT( &app->owner ));
		    goto done;
		}
		sca_appearance_free( app );

		sca_hash_table_unlock_index( sca->appearances, slot_idx );
		slot_idx = -1;

		if ( sca_notify_call_info_subscribers( sca, from_aor ) < 0 ) {
		    LM_ERR( "Failed to call-info NOTIFY %.*s subscribers "
			    "on BYE", STR_FMT( &to->uri ));
		    goto done;
		}
	    }
	} else {
	    if ( !sca_uri_lock_if_shared_appearance( sca, to_aor, &slot_idx )) {
		LM_DBG( "BYE from non-SCA %.*s to non-SCA %.*s",
			STR_FMT( from_aor ), STR_FMT( to_aor ));
		rc = 1;
		goto done;
	    }

	    app = sca_appearance_for_tags_unsafe( sca, to_aor,
			&msg->callid->body, &to->tag_value,
			NULL, slot_idx );
	    if ( app == NULL ) {
		LM_ERR( "sca_call_info_bye_handler: failed to look up "
			"dialog for BYE %.*s from %.*s",
			STR_FMT( to_aor ), STR_FMT( from_aor ));
		goto done;
	    }

	    if (( app->flags & SCA_APPEARANCE_FLAG_OWNER_PENDING )) {
LM_INFO( "ADMORTEN DEBUG: %.*s appearance-index %d has owner change pending",
	 STR_FMT( &app->owner ), app->index );
		/* ... */
	    } else {
		unl_app = sca_appearance_list_unlink_index(
					app->appearance_list, app->index );
		if ( unl_app == NULL || unl_app != app ) {
		    LM_ERR( "sca_call_info_bye_handler: failed to unlink "
			    "%.*s appearance-index %d, owner %.*s",
			    STR_FMT( &app->owner ), app->index,
			    STR_FMT( &app->owner ));
		    goto done;
		}
		sca_appearance_free( app );

		sca_hash_table_unlock_index( sca->appearances, slot_idx );
		slot_idx = -1;

		if ( sca_notify_call_info_subscribers( sca, to_aor ) < 0 ) {
		    LM_ERR( "Failed to call-info NOTIFY %.*s subscribers "
			    "on BYE", STR_FMT( to_aor ));
		    goto done;
		}
	    }
	}
    } else {
	if ( call_info != NULL ) {
	    slot_idx = sca_hash_table_index_for_key( sca->appearances, to_aor );
	    sca_hash_table_lock_index( sca->appearances, slot_idx );

	    app = sca_appearance_for_index_unsafe( sca, to_aor,
			call_info->index, slot_idx );
	    if ( app == NULL ) {
		LM_ERR( "sca_call_info_bye_handler: failed to look up "
			"dialog for BYE %.*s from %.*s",
			STR_FMT( to_aor ), STR_FMT( from_aor ));
		goto done;
	    }

	    if (( app->flags & SCA_APPEARANCE_FLAG_OWNER_PENDING )) {
LM_INFO( "ADMORTEN DEBUG: %.*s appearance-index %d has owner change pending",
	 STR_FMT( &app->owner ), app->index );
		if ( SCA_STR_EQ( &app->dialog.call_id, &msg->callid->body ) &&
			SCA_STR_EQ( &app->dialog.from_tag, &from->tag_value ) &&
			SCA_STR_EQ( &app->dialog.to_tag, &to->tag_value )) {
		    LM_INFO( "ADMORTEN DEBUG: match" );
		}
	    } else {
		unl_app = sca_appearance_list_unlink_index(
					app->appearance_list, app->index );
		if ( unl_app == NULL || unl_app != app ) {
		    LM_ERR( "sca_call_info_bye_handler: failed to unlink "
			    "%.*s appearance-index %d, owner %.*s",
			    STR_FMT( &app->owner ), app->index,
			    STR_FMT( &app->owner ));
		    goto done;
		}
		sca_appearance_free( app );
		
		sca_hash_table_unlock_index( sca->appearances, slot_idx );
		slot_idx = -1;

		if ( sca_notify_call_info_subscribers( sca, to_aor ) < 0 ) {
		    LM_ERR( "Failed to call-info NOTIFY %.*s subscribers "
			    "on BYE", STR_FMT( to_aor ));
		    goto done;
		}
	    }
	}
    }

    rc = 1;

done:
    if ( slot_idx >= 0 ) {
	sca_hash_table_unlock_index( sca->appearances, slot_idx );
    }

    return( rc );
}

    static int
sca_call_info_cancel_handler( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to,
	str *from_aor, str *to_aor, str *contact_uri )
{
    sca_appearance	*app;
    int			rc = 1;

    if ( msg->first_line.type != SIP_REQUEST ) {
	return( 1 );
    }

    /*
     * Polycom SCA CANCELs as of sip.ld 3.3.4 don't include Call-Info headers;
     * find appearance by dialog if Call-Info not present.
     */
    /* XXX also handle CANCEL w/ Call-Info header? Some UAs might send it */
    if ( sca_uri_is_shared_appearance( sca, from_aor )) {
	app = sca_appearance_unlink_by_tags( sca, from_aor,
			&msg->callid->body, &from->tag_value, NULL );
	if ( app ) {
	    sca_appearance_free( app );

	    if ( sca_notify_call_info_subscribers( sca, from_aor ) < 0 ) {
		LM_ERR( "Failed to call-info NOTIFY %.*s subscribers on CANCEL",
			STR_FMT( from_aor ));
		rc = -1;
	    }
	}
    }

    if ( !SCA_STR_EMPTY( &to->tag_value ) &&
		sca_uri_is_shared_appearance( sca, to_aor )) {
	app = sca_appearance_unlink_by_tags( sca, to_aor,
			&msg->callid->body, &to->tag_value, NULL );
	if ( app ) {
	    sca_appearance_free( app );

	    if ( sca_notify_call_info_subscribers( sca, to_aor ) < 0 ) {
		LM_ERR( "Failed to call-info NOTIFY %.*s subscribers on CANCEL",
			STR_FMT( to_aor ));
		rc = -1;
	    }
	}
    }

    return( rc );
}

    void
sca_call_info_sl_reply_cb( void *cb_arg )
{
    sl_cbp_t		*slcbp = (sl_cbp_t *)cb_arg;
    sip_msg_t		*msg;
    struct to_body	*from;
    struct to_body	*to;
    str			aor = STR_NULL;
    str			contact_uri = STR_NULL;

    if ( slcbp == NULL ) {
	LM_INFO( "ADMORTEN DEBUG: sca_call_info_sl_reply_cb: slcbp is NULL" );
	return;
    }

    if ( slcbp->type != SLCB_REPLY_READY ) {
	return;
    }

    /* for now, it appears we only need this during INVITEs... */
    if ( slcbp->req->REQ_METHOD != METHOD_INVITE ) {
	return;
    }

    /* ...and even then only on error */
    if ( slcbp->code < 400 || slcbp->code == 401 || slcbp->code == 407 ) {
	/* LM_DBG( "sca_call_info_sl_reply_cb: ignoring stateless reply with "
		"status %d %.*s", slcbp->code, STR_FMT( slcbp->reason )); */
	return;
    }

    msg = slcbp->req;
    if ( sca_get_msg_from_header( msg, &from ) < 0 ) {
	LM_ERR( "sca_call_info_sl_reply_cb: failed to get From header from "
		"request before stateless reply with %d %.*s",
		slcbp->code, STR_FMT( slcbp->reason ));
	return;
    }
    if ( sca_uri_extract_aor( &from->uri, &aor ) < 0 ) {
	LM_ERR( "sca_call_info_sl_reply_cb: failed to extract AoR "
		"from URI %.*s", STR_FMT( &from->uri ));
	return;
    }

    if ( !sca_uri_is_shared_appearance( sca, &aor )) {
	/* LM_DBG( "sca_call_info_sl_reply_cb: ignoring non-shared appearance "
		"%.*s", STR_FMT( &aor )); */
	return;
    }

    if ( sca_get_msg_contact_uri( msg, &contact_uri ) < 0 ) {
	LM_ERR( "sca_call_info_sl_reply_cb: failed to get Contact from "
		"request before stateless reply with %d %.*s",
		slcbp->code, STR_FMT( slcbp->reason ));
	return;
    }

    if ( sca_get_msg_to_header( msg, &to ) < 0 ) {
	LM_ERR( "sca_call_info_sl_reply_cb: failed to get To header from "
		"request before stateless reply with %d %.*s",
		slcbp->code, STR_FMT( slcbp->reason ));
	return;
    }

    if ( sca_subscription_terminate( sca, &aor,
		SCA_EVENT_TYPE_LINE_SEIZE, &contact_uri,
		SCA_SUBSCRIPTION_STATE_TERMINATED_NORESOURCE,
		SCA_SUBSCRIPTION_TERMINATE_OPT_DEFAULT ) < 0 ) {
	LM_ERR( "sca_call_info_sl_reply_cb: failed to terminate "
		"line-seize subscription for %.*s", STR_FMT( &contact_uri ));
	return;
    }
}

    static inline int
sca_call_info_prack_handler( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor,
	str *to_aor, str *contact_uri )
{
    return( 1 );
}

    static inline int
sca_call_info_refer_handler( sip_msg_t *msg, sca_call_info *call_info,
	struct to_body *from, struct to_body *to, str *from_aor,
	str *to_aor, str *contact_uri )
{
    return( 1 );
}


struct sca_call_info_dispatch {
    int			method;
    int			(*handler)( sip_msg_t *, sca_call_info *,
				    struct to_body *, struct to_body *,
				    str *, str *, str * );
};
struct sca_call_info_dispatch	call_info_dispatch[] = {
    { METHOD_BYE,	sca_call_info_bye_handler },
    { METHOD_CANCEL,	sca_call_info_cancel_handler },
    { METHOD_INVITE,	sca_call_info_invite_handler },
    { METHOD_PRACK,	sca_call_info_prack_handler },
    { METHOD_REFER,	sca_call_info_refer_handler },
};

#define SCA_CALL_INFO_UPDATE_FLAG_DEFAULT	0
#define SCA_CALL_INFO_UPDATE_FLAG_FROM_ALLOC	(1 << 0)
#define SCA_CALL_INFO_UPDATE_FLAG_TO_ALLOC	(1 << 1)
    int
sca_call_info_update( sip_msg_t *msg, char *p1, char *p2 )
{
    sca_call_info	call_info, *call_info_p = NULL;
    hdr_field_t		*call_info_hdr;
    struct to_body	*from;
    struct to_body	*to;
    sip_uri_t		c_uri;
    str			from_aor = STR_NULL;
    str			to_aor = STR_NULL;
    str			contact_uri = STR_NULL;
    int			aor_flags = SCA_CALL_INFO_UPDATE_FLAG_DEFAULT;
    int			n_dispatch;
    int			i;
    int			method;
    int			rc = -1;

    method = sca_get_msg_method( msg );

    n_dispatch = sizeof( call_info_dispatch ) / sizeof( call_info_dispatch[0] );
    for ( i = 0; i < n_dispatch; i++ ) {
	if ( method == call_info_dispatch[ i ].method ) {
	    break;
	}
    }
    if ( i >= n_dispatch ) {
	LM_DBG( "BUG: sca module does not support Call-Info headers "
		"in %.*s requests", STR_FMT( &get_cseq( msg )->method ));
	return( 1 );
    }

    if ( parse_headers( msg, HDR_EOH_F, 0 ) < 0 ) {
	LM_ERR( "header parsing failed: bad request" );
	return( -1 );
    }

    memset( &call_info, 0, sizeof( sca_call_info ));
    call_info_hdr = sca_call_info_header_find( msg->headers );
    if ( !SCA_HEADER_EMPTY( call_info_hdr )) {
	/* this needs to accomodate comma-separated appearance info */
	if ( sca_call_info_body_parse( &call_info_hdr->body, &call_info ) < 0) {
	    LM_ERR( "Bad Call-Info header body: %.*s",
		    STR_FMT( &call_info_hdr->body ));
	    return( -1 );
	}

	call_info_p = &call_info;
    }

    if ( sca_get_msg_from_header( msg, &from ) < 0 ) {
	LM_ERR( "Bad From header" );
	return( -1 );
    }
    if ( sca_get_msg_to_header( msg, &to ) < 0 ) {
	LM_ERR( "Bad To header" );
	return( -1 );
    }

    memset( &c_uri, 0, sizeof( sip_uri_t ));
    rc = sca_get_msg_contact_uri( msg, &contact_uri );
    if ( rc > 0 ) {
	/* Contact header in packet */
	if ( parse_uri( contact_uri.s, contact_uri.len, &c_uri ) < 0 ) {
	    LM_ERR( "Failed to parse Contact URI %.*s",
		    STR_FMT( &contact_uri ));
	    return( -1 );
	}
    } else if ( rc < 0 ) {
	LM_ERR( "Bad Contact" );
	return( -1 );
    }
    /* reset rc to -1 so we don't end up returning 0 to the script */
    rc = -1;

    if ( sca_call_info_header_remove( msg ) < 0 ) {
	LM_ERR( "Failed to remove Call-Info header" );
	return( -1 );
    }

    /* reconcile mismatched Contact users and To/From URIs */
    if ( sca_uri_extract_aor( &from->uri, &from_aor ) < 0 ) {
	LM_ERR( "sca_uri_extract_aor failed to extract AoR from From URI %.*s",
		STR_FMT( &from->uri ));
	goto done;
    }
    if ( sca_uri_extract_aor( &to->uri, &to_aor ) < 0 ) {
	LM_ERR( "sca_uri_extract_aor failed to extract AoR from To URI %.*s",
		STR_FMT( &to->uri ));
	goto done;
    }

    if ( !SCA_STR_EMPTY( &c_uri.user )) {
	if ( msg->first_line.type == SIP_REQUEST ) {
	    if ( !SCA_STR_EQ( &c_uri.user, &GET_FROM_PURI( msg )->user )) {
		if ( sca_aor_create_from_info( &from_aor, c_uri.type,
			&c_uri.user, &GET_FROM_PURI( msg )->host,
			&GET_FROM_PURI( msg )->port ) < 0 ) {
		    LM_ERR( "sca_aor_create_from_info from Contact %.*s "
			    "and From URI %.*s failed",
			    STR_FMT( &contact_uri ), STR_FMT( &from->uri ));
		    goto done;
		}

		aor_flags |= SCA_CALL_INFO_UPDATE_FLAG_FROM_ALLOC;
	    }
	} else {
	    if ( !SCA_STR_EQ( &c_uri.user, &GET_TO_PURI( msg )->user )) {
		if ( sca_aor_create_from_info( &to_aor, c_uri.type,
			&c_uri.user, &GET_TO_PURI( msg )->host,
			&GET_TO_PURI( msg )->port ) < 0 ) {
		    LM_ERR( "sca_aor_create_from_info from Contact %.*s "
			    "and To URI %.*s failed", STR_FMT( &contact_uri ),
			    STR_FMT( &from->uri ));
		    goto done;
		} 

		aor_flags |= SCA_CALL_INFO_UPDATE_FLAG_TO_ALLOC;
	    }
	}
    }

    /* early check to see if we're dealing with any SCA endpoints */
    if ( call_info_p == NULL ) {
	/* no Call-Info header in packet, check if AoRs are SCA */

	if ( !sca_uri_is_shared_appearance( sca, &from_aor ) &&
		    !sca_uri_is_shared_appearance( sca, &to_aor )) {
	    LM_DBG( "Neither %.*s nor %.*s are SCA AoRs",
		    STR_FMT( &from_aor ), STR_FMT( &to_aor ));
	    goto done;
	}
    }

    rc = call_info_dispatch[ i ].handler( msg, call_info_p, from, to,
					&from_aor, &to_aor, &contact_uri );
    if ( rc < 0 ) {
	LM_ERR( "Failed to update Call-Info state for %.*s",
		STR_FMT( &contact_uri ));
    }

done:
    if (( aor_flags & SCA_CALL_INFO_UPDATE_FLAG_FROM_ALLOC )) {
	if ( from_aor.s != NULL ) {
	    pkg_free( from_aor.s );
	}
    }
    if (( aor_flags & SCA_CALL_INFO_UPDATE_FLAG_TO_ALLOC )) {
	if ( to_aor.s != NULL ) {
	    pkg_free( to_aor.s );
	}
    }

    return( rc );
}
