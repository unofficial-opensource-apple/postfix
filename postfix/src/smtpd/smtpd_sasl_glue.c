/*++
/* NAME
/*	smtpd_sasl_glue 3
/* SUMMARY
/*	Postfix SMTP server, SASL support interface
/* SYNOPSIS
/*	#include "smtpd_sasl_glue.h"
/*
/*	void    smtpd_sasl_initialize()
/*
/*	void	smtpd_sasl_connect(state, sasl_opts_name, sasl_opts_val)
/*	SMTPD_STATE *state;
/*	const char *sasl_opts_name;
/*	const char *sasl_opts_val;
/*
/*	char	*smtpd_sasl_authenticate(state, sasl_method, init_response)
/*	SMTPD_STATE *state;
/*	const char *sasl_method;
/*	const char *init_response;
/*
/*	void	smtpd_sasl_logout(state)
/*	SMTPD_STATE *state;
/*
/*	void	smtpd_sasl_disconnect(state)
/*	SMTPD_STATE *state;
/* DESCRIPTION
/*	This module encapsulates most of the detail specific to SASL
/*	authentication.
/*
/*	smtpd_sasl_initialize() initializes the SASL library. This
/*	routine should be called once at process start-up. It may
/*	need access to the file system for run-time loading of
/*	plug-in modules. There is no corresponding cleanup routine.
/*
/*	smtpd_sasl_connect() performs per-connection initialization.
/*	This routine should be called once at the start of every
/*	connection. The sasl_opts_name and sasl_opts_val parameters
/*	are the postfix configuration parameters setting the security
/*	policy of the SASL authentication.
/*
/*	smtpd_sasl_authenticate() implements the authentication
/*	dialog.  The result is zero in case of success, -1 in case
/*	of failure. smtpd_sasl_authenticate() updates the following
/*	state structure members:
/* .IP sasl_method
/*	The authentication method that was successfully applied.
/*	This member is a null pointer in the absence of successful
/*	authentication.
/* .IP sasl_username
/*	The username that was successfully authenticated.
/*	This member is a null pointer in the absence of successful
/*	authentication.
/* .PP
/*	smtpd_sasl_logout() cleans up after smtpd_sasl_authenticate().
/*	This routine exists for the sake of symmetry.
/*
/*	smtpd_sasl_disconnect() performs per-connection cleanup.
/*	This routine should be called at the end of every connection.
/*
/*	Arguments:
/* .IP state
/*	SMTP session context.
/* .IP sasl_opts_name
/*	Security options parameter name.
/* .IP sasl_opts_val
/*	Security options parameter value.
/* .IP sasl_method
/*	A SASL mechanism name
/* .IP init_reply
/*	An optional initial client response.
/* DIAGNOSTICS
/*	All errors are fatal.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Initial implementation by:
/*	Till Franke
/*	SuSE Rhein/Main AG
/*	65760 Eschborn, Germany
/*
/*	Adopted by:
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE_OS_X_SERVER__
#include <syslog.h>
#include <stdio.h>
#include <sys/stat.h>
#endif /* __APPLE_OS_X_SERVER__ */

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>
#include <stringops.h>

/* Global library. */

#include <mail_params.h>

/* XSASL library. */

#include <xsasl.h>

/* Application-specific. */

#include "smtpd.h"
#include "smtpd_sasl_glue.h"
#include "smtpd_chat.h"

#ifdef __APPLE_OS_X_SERVER__
/* Apple Open Directory */

#include <OpenDirectory/OpenDirectory.h>
#include <OpenDirectory/OpenDirectoryPriv.h>
#include <DirectoryService/DirServicesTypes.h>

#include <DirectoryService/DirServices.h>
#include <DirectoryService/DirServicesUtils.h>
#include <DirectoryService/DirServicesConst.h>

#include <CoreFoundation/CFData.h>
#include <CoreFoundation/CFString.h>
#include <CoreFoundation/CFNumber.h>
#include <CoreFoundation/CFPropertyList.h>

/* kerberos */
//#include <Kerberos/Kerberos.h>
#include <Kerberos/gssapi_krb5.h>
//#include <Kerberos/gssapi.h>

/* mach */

#include <mach/boolean.h>
#endif /* __APPLE_OS_X_SERVER__ */

#ifdef USE_SASL_AUTH

/*
 * Silly little macros.
 */
#define STR(s)	vstring_str(s)

 /*
  * SASL server implementation handle.
  */
static XSASL_SERVER_IMPL *smtpd_sasl_impl;

#ifdef __APPLE_OS_X_SERVER__

/* Apple's Password Server */
static NAME_MASK smtpd_pw_server_mask[] = {
    "none",     PW_SERVER_NONE,
    "login",    PW_SERVER_LOGIN,
    "plain",    PW_SERVER_PLAIN,
    "cram-md5", PW_SERVER_CRAM_MD5,
    "gssapi",   PW_SERVER_GSSAPI,
    0,
};

ODSessionRef		od_session_ref;
ODNodeRef			od_node_ref;

#endif /* __APPLE_OS_X_SERVER__ */

/* smtpd_sasl_initialize - per-process initialization */

#ifdef __APPLE_OS_X_SERVER__
void    smtpd_sasl_initialize( int in_use_pw_server )
{
	if ( in_use_pw_server )
	{
		smtpd_pw_server_sasl_opts = name_mask( VAR_SMTPD_PW_SERVER_OPTS, smtpd_pw_server_mask,
									 var_smtpd_pw_server_opts );
	}
#else /* __APPLE_OS_X_SERVER__ */
void    smtpd_sasl_initialize(void)
{
#endif /* __APPLE_OS_X_SERVER__ */

    /*
     * Sanity check.
     */
    if (smtpd_sasl_impl)
	msg_panic("smtpd_sasl_initialize: repeated call");

    /*
     * Initialize the SASL library.
     */
    if ((smtpd_sasl_impl = xsasl_server_init(var_smtpd_sasl_type,
					     var_smtpd_sasl_path)) == 0)
	msg_fatal("SASL per-process initialization failed");

}

/* smtpd_sasl_connect - per-connection initialization */

void    smtpd_sasl_connect(SMTPD_STATE *state, const char *sasl_opts_name,
			           const char *sasl_opts_val)
{
    const char *mechanism_list;

    /*
     * Initialize SASL-specific state variables. Use long-lived storage for
     * base 64 conversion results, rather than local variables, to avoid
     * memory leaks when a read or write routine returns abnormally after
     * timeout or I/O error.
     */
    state->sasl_reply = vstring_alloc(20);
    state->sasl_mechanism_list = 0;
    state->sasl_username = 0;
    state->sasl_method = 0;
    state->sasl_sender = 0;

    /*
     * Set up a new server context for this connection.
     */
#define SMTPD_SASL_SERVICE "smtp"

    if ((state->sasl_server =
	 xsasl_server_create(smtpd_sasl_impl, state->client,
			     SMTPD_SASL_SERVICE, *var_smtpd_sasl_realm ?
			     var_smtpd_sasl_realm : (char *) 0,
			     sasl_opts_val)) == 0)
	msg_fatal("SASL per-connection initialization failed");

    /*
     * Get the list of authentication mechanisms.
     */
    if ((mechanism_list =
	 xsasl_server_get_mechanism_list(state->sasl_server)) == 0)
	msg_fatal("no SASL authentication mechanisms");
    state->sasl_mechanism_list = mystrdup(mechanism_list);
}

/* smtpd_sasl_disconnect - per-connection cleanup */

void    smtpd_sasl_disconnect(SMTPD_STATE *state)
{
    if (state->sasl_reply) {
	vstring_free(state->sasl_reply);
	state->sasl_reply = 0;
    }
    if (state->sasl_mechanism_list) {
	myfree(state->sasl_mechanism_list);
	state->sasl_mechanism_list = 0;
    }
    if (state->sasl_username) {
	myfree(state->sasl_username);
	state->sasl_username = 0;
    }
    if (state->sasl_method) {
	myfree(state->sasl_method);
	state->sasl_method = 0;
    }
    if (state->sasl_sender) {
	myfree(state->sasl_sender);
	state->sasl_sender = 0;
    }
    if (state->sasl_server) {
	xsasl_server_free(state->sasl_server);
	state->sasl_server = 0;
    }
}

/* smtpd_sasl_authenticate - per-session authentication */

int     smtpd_sasl_authenticate(SMTPD_STATE *state,
				        const char *sasl_method,
				        const char *init_response)
{
    int     status;
    const char *sasl_username;

    /*
     * SASL authentication protocol start-up. Process any initial client
     * response that was sent along in the AUTH command.
     */
    for (status = xsasl_server_first(state->sasl_server, sasl_method,
				     init_response, state->sasl_reply);
	 status == XSASL_AUTH_MORE;
	 status = xsasl_server_next(state->sasl_server, STR(state->buffer),
				    state->sasl_reply)) {

	/*
	 * Send a server challenge.
	 */
	smtpd_chat_reply(state, "334 %s", STR(state->sasl_reply));

	/*
	 * Receive the client response. "*" means that the client gives up.
	 * XXX For now we ignore the fact that an excessively long response
	 * will be chopped into multiple reponses. To handle such responses,
	 * we need to change smtpd_chat_query() so that it returns an error
	 * indication.
	 */
	smtpd_chat_query(state);
	if (strcmp(STR(state->buffer), "*") == 0) {
	    msg_warn("%s: SASL %s authentication aborted",
		     state->namaddr, sasl_method);
	    smtpd_chat_reply(state, "501 5.7.0 Authentication aborted");
	    return (-1);
	}
    }
    if (status != XSASL_AUTH_DONE) {
	msg_warn("%s: SASL %s authentication failed: %s",
		 state->namaddr, sasl_method,
		 STR(state->sasl_reply));
	/* RFC 4954 Section 6. */
	smtpd_chat_reply(state, "535 5.7.8 Error: authentication failed: %s",
			 STR(state->sasl_reply));
	return (-1);
    }
    /* RFC 4954 Section 6. */
    smtpd_chat_reply(state, "235 2.7.0 Authentication successful");
    if ((sasl_username = xsasl_server_get_username(state->sasl_server)) == 0)
	msg_panic("cannot look up the authenticated SASL username");
    state->sasl_username = mystrdup(sasl_username);
    printable(state->sasl_username, '?');
    state->sasl_method = mystrdup(sasl_method);
    printable(state->sasl_method, '?');

    return (0);
}

/* smtpd_sasl_logout - clean up after smtpd_sasl_authenticate */

void    smtpd_sasl_logout(SMTPD_STATE *state)
{
    if (state->sasl_username) {
	myfree(state->sasl_username);
	state->sasl_username = 0;
    }
    if (state->sasl_method) {
	myfree(state->sasl_method);
	state->sasl_method = 0;
    }
}

#ifdef __APPLE_OS_X_SERVER__
/* -----------------------------------------------------------------
	- Password Server auth methods
   ----------------------------------------------------------------- */

#include <sys/param.h>

static bool			od_open					( void );
static int			od_do_clear_text_auth	( const char *in_user, const char *in_passwd );
static int			od_validate_response	( const char *in_user, const char *in_chal, const char *in_resp, const char *in_auth_type );
static ODRecordRef	od_get_user_record		( const char *in_user );
static void			print_cf_error			( CFErrorRef in_cf_err_ref, const char *in_default_str );

static char		   *do_auth_login			( SMTPD_STATE *state, const char *in_method );
static char		   *do_auth_plain			( SMTPD_STATE *state, const char *in_method, const char *in_resp );
static char		   *do_auth_cram_md5		( SMTPD_STATE *state, const char *in_method );

static void			get_random_chars		( char *out_buf, int in_len );


/* -----------------------------------------------------------------
	- smtpd_pw_server_authenticate
   ----------------------------------------------------------------- */

char *smtpd_pw_server_authenticate ( SMTPD_STATE *state, const char *in_method, const char *in_resp )
{
    char *myname = "smtpd_pw_server_authenticate";

	/*** Sanity check ***/
    if ( state->sasl_username || state->sasl_method )
	{
		msg_panic( "%s: already authenticated", myname );
	}

	if ( strcasecmp( in_method, "LOGIN" ) == 0 )
	{
		return( do_auth_login( state, in_method ) );
	}
	else if ( strcasecmp( in_method, "PLAIN" ) == 0 )
	{
		return( do_auth_plain( state, in_method, in_resp ) );
	}
	else if ( strcasecmp( in_method, "CRAM-MD5" ) == 0 )
	{
		return( do_auth_cram_md5( state, in_method ) );
	}

	msg_error( "Authentication method: &s is not supported", in_method );
	return ( "504 Unsupported authentication method" );

} /* smtpd_pw_server_authenticate */


/* ------------------------------------------------------------------
	- print_cf_error ()
   ------------------------------------------------------------------ */

static void print_cf_error ( CFErrorRef in_cf_err_ref, const char *in_default_str )
{
	char			c_str[1024 + 1];
	CFStringRef		cf_str_ref		= NULL;

	if ( in_cf_err_ref != NULL )
	{
		cf_str_ref = CFErrorCopyFailureReason( in_cf_err_ref );
		if ( cf_str_ref != NULL )
		{
			CFStringGetCString( cf_str_ref, c_str, 1024, kCFStringEncodingUTF8 );

			msg_error( "CF: %s", c_str );
			return;
		}
	}

	msg_error( "%s", in_default_str );

} /* print_cf_error */


/* -----------------------------------------------------------------
	- get_random_chars
   ----------------------------------------------------------------- */

static void get_random_chars ( char *out_buf, int in_len )
{
    int					count = 0;
    int					file;
	unsigned long long	microseconds = 0ULL;
	struct timeval		tv;
	struct timezone		tz;
    
    memset( out_buf, 0, in_len );

	/* try to open /dev/urandom */
    file = open( "/dev/urandom", O_RDONLY, 0 );
    if ( file == -1 )
	{
		syslog( LOG_ERR, "Cannot open /dev/urandom" );

		/* try to open /dev/random */
		file = open( "/dev/random", O_RDONLY, 0 );
	}

    if ( file == -1 )
    {
		syslog( LOG_ERR, "Cannot open /dev/random" );

		gettimeofday( &tv, &tz );

		microseconds = (unsigned long long)tv.tv_sec;
		microseconds *= 1000000ULL;
		microseconds += (unsigned long long)tv.tv_usec;

		snprintf( out_buf, in_len, "%llu", microseconds );
    }
	else
	{
		/* make sure the chars are printable */
		while ( count < (in_len - 1) )
		{
			read( file, &out_buf[ count ], 1 );
			if ( isalnum( out_buf[ count ] ) )
			{
				count++;
			}
		}
		close( file );
	}
} /* get_random_chars */


/* -----------------------------------------------------------------
	- do_auth_login
   ----------------------------------------------------------------- */

static char * do_auth_login ( SMTPD_STATE *state, const char *in_method )
{
    static VSTRING	*vs_base64	= 0;
    static VSTRING	*vs_user	= 0;
    static VSTRING	*vs_pwd		= 0;

	vs_base64 = vstring_alloc(10);
	vs_user = vstring_alloc(10);
	vs_pwd = vstring_alloc(10);

	/* is LOGIN auth enabled */
	if ( !(smtpd_pw_server_sasl_opts & PW_SERVER_LOGIN) )
	{
		msg_error( "Authentication method: LOGIN is not enabled" );
		return( "504 Authentication method not enabled" );
	}

	/* encode the user name prompt and send it */
	base64_encode( vs_base64, "Username:", 9 );
	smtpd_chat_reply( state, "334 %s", STR(vs_base64) );

	/* get the user name and decode it */
	smtpd_chat_query( state );

	/* has the client given up */
	if ( strcmp(vstring_str( state->buffer ), "*") == 0 )
	{
		msg_error( "Authentication aborted by client" );
		return ( "501 Authentication aborted" );
	}

	/* decode user name */
    if ( base64_decode( vs_user, STR(state->buffer), VSTRING_LEN(state->buffer) ) == 0 )
	{
		msg_error( "Malformed response to: AUTH LOGIN" );
		return( "501 Authentication failed: malformed initial response" );
	}

	/* encode the password prompt and send it */
	base64_encode( vs_base64, "Password:", 9 );
	smtpd_chat_reply( state, "334 %s", STR(vs_base64) );

	/* get the password */
	smtpd_chat_query( state );

	/* has the client given up */
	if ( strcmp(vstring_str( state->buffer ), "*") == 0 )
	{
		msg_error( "Authentication aborted by client" );
		return ( "501 Authentication aborted" );
	}

	/* decode the password */
    if ( base64_decode( vs_pwd, STR(state->buffer), VSTRING_LEN(state->buffer) ) == 0 )
	{
		msg_error( "Malformed response to: AUTH LOGIN" );
		return ( "501 Authentication failed: malformed response" );
	}

	/* do the auth */
	if ( od_do_clear_text_auth( STR(vs_user), STR(vs_pwd) ) == eAOD_no_error )
	{
		state->sasl_username = mystrdup( STR(vs_user) );
		state->sasl_method = mystrdup( in_method );

		return( NULL );
	}
	else
	{
		msg_error( "Authentication failed" );
		return ( "535 Error: authentication failed" );
	}

} /* do_auth_login */


/* -----------------------------------------------------------------
	- do_auth_plain
   ----------------------------------------------------------------- */

static char *do_auth_plain ( SMTPD_STATE *state, const char *in_method, const char *in_resp )
{
	char			*ptr		= NULL;
	char			*p_user		= NULL;
	char			*p_pwd		= NULL;
    static VSTRING	*vs_base64	= 0;

	vs_base64 = vstring_alloc(10);

	/* is PLAIN auth enabled */
	if ( !(smtpd_pw_server_sasl_opts & PW_SERVER_PLAIN) )
	{
		msg_error( "Authentication method: PLAIN is not enabled" );
		return ( "504 Authentication method not enabled" );
	}

	/* if no initial response, do the dance */
	if ( in_resp == NULL )
	{
		/* send 334 tag & read response */
		smtpd_chat_reply( state, "334" );
		smtpd_chat_query( state );

		/* decode response from server */
		if ( base64_decode( vs_base64, STR(state->buffer), VSTRING_LEN(state->buffer) ) == 0 )
		{
			msg_error( "Malformed response to: AUTH PLAIN" );
			return ( "501 Authentication failed: malformed initial response" );
		}
	}
	else
	{
		/* decode response from server */
		if ( base64_decode( vs_base64, in_resp, strlen( in_resp ) ) == 0 )
		{
			msg_error( "Malformed response to: AUTH PLAIN" );
			return ( "501 Authentication failed: malformed initial response" );
		}
	}


	ptr = STR(vs_base64);
	if ( *ptr == '\0' )
	{
		ptr++;
	}

	if ( ptr != NULL )
	{
		/* point to user portion in the digest */
		p_user = ptr;

		ptr = ptr + (strlen( p_user ) + 1 );
		if ( ptr != NULL )
		{
			/* point to password portion in the digest */
			p_pwd = ptr;

			/* do the auth */
			if ( od_do_clear_text_auth( p_user, p_pwd ) == eAOD_no_error )
			{
				state->sasl_username = mystrdup( p_user );
				state->sasl_method = mystrdup( in_method );

				return( NULL );
			}
		}
	}

	return ( "535 Error: authentication failed" );

} /* do_auth_plain */


/* -----------------------------------------------------------------
	- do_auth_cram_md5
   ----------------------------------------------------------------- */

static char *do_auth_cram_md5 ( SMTPD_STATE *state, const char *in_method )
{
	int				len			= 0;
	char			*ptr		= NULL;
	char			*resp_ptr	= NULL;
    static VSTRING	*vs_base64	= 0;
    static VSTRING	*vs_chal	= 0;
    static VSTRING	*vs_user	= 0;
	char			 rand_buf[ 17 ];
	char			 host_name[ MAXHOSTNAMELEN + 1 ];

	vs_base64 = vstring_alloc(10);
	vs_chal = vstring_alloc(10);
	vs_user = vstring_alloc(10);

	/* is CRAM-MD5 auth enabled */
	if ( !(smtpd_pw_server_sasl_opts & PW_SERVER_CRAM_MD5) )
	{
		msg_error( "Authentication method: CRAM-MD5 is not enabled" );
		return ( "504 Authentication method not enabled" );
	}

	/* challenge host name */
	gethostname( host_name, sizeof( host_name ) );

	/* get random data string */
	get_random_chars( rand_buf, 17 );

	/* now make the challenge string */
	vstring_sprintf( vs_chal, "<%lu.-%s.-%lu-@-%s>", getpid(), rand_buf, time(0), host_name );

	/* encode the challenge and send it */
	base64_encode( vs_base64, STR(vs_chal),  VSTRING_LEN(vs_chal) );
	smtpd_chat_reply( state, "334 %s", STR(vs_base64) );

	/* get the client response */
	smtpd_chat_query( state );

	/* check if client cancelled */
	if ( strcmp( vstring_str( state->buffer ), "*" ) == 0 )
	{
		return( "501 Authentication aborted" );
	}

	/* decode the response */
	if ( base64_decode( vs_base64, STR(state->buffer), VSTRING_LEN(state->buffer) ) == 0 )
	{
		msg_error( "Malformed response to: AUTH CRAM-MD5" );
		return( "501 Authentication failed: malformed initial response" );
	}

	/* get the user name */
	resp_ptr = STR(vs_base64);
	ptr = strchr( resp_ptr, ' ' );
	if ( ptr != NULL )
	{
		/* copy user name */
		len = ptr - resp_ptr;
		vs_user = vstring_strncpy( vs_user, resp_ptr, len );

		/* move past the space */
		ptr++;
		if ( ptr != NULL )
		{
			/* validate the response */

			if ( od_validate_response( STR(vs_user), STR(vs_chal), ptr, kDSStdAuthCRAM_MD5 ) == eAOD_no_error )
			{
				state->sasl_username = mystrdup( STR(vs_user) );
				state->sasl_method = mystrdup( in_method );

				return( NULL );
			}
		}
	}

	return ( "535 Error: authentication failed" );

} /* do_auth_cram_md5 */


/* -----------------------------------------------------------------
	od_do_clear_text_auth ()
   ----------------------------------------------------------------- */

static int od_do_clear_text_auth ( const char *in_user, const char *in_passwd )
{
	int						out_result		= eAOD_auth_failed;
	CFErrorRef				cf_err_ref		= NULL;
	ODRecordRef				od_rec_ref		= NULL;
	CFStringRef				cf_str_user		= NULL;
	CFStringRef				cf_str_pwd		= NULL;

	if ( (in_user == NULL) || (in_passwd == NULL) )
	{
		return( eAOD_param_error );
	}

	if ( od_open() == FALSE )
	{
		return( eAOD_open_OD_failed );
	}

	od_rec_ref = od_get_user_record( in_user );
	if ( od_rec_ref == NULL )
	{
		/* print the error and bail */
		print_cf_error( cf_err_ref, "Unable to lookup user record" );

		/* release OD session */
		return( eAOD_unknown_user );
	}

	cf_str_pwd = CFStringCreateWithCString( NULL, in_passwd, kCFStringEncodingUTF8 );
	if ( cf_str_pwd == NULL )
	{
		CFRelease( od_rec_ref );
		msg_error( "Unable to create user name CFStringRef" );
		return;
	}

	if ( ODRecordVerifyPassword( od_rec_ref, cf_str_pwd, &cf_err_ref ) )
	{
		out_result = eAOD_no_error;
	}
	else
	{
		if ( cf_err_ref != NULL )
		{
			print_cf_error( cf_err_ref, "Auth failed" );
		}
		out_result = eAOD_passwd_mismatch;
	}

	/* do some cleanup */
	if ( cf_err_ref != NULL )
	{
		CFRelease( cf_err_ref );
	}

	if ( od_rec_ref != NULL )
	{
		CFRelease( od_rec_ref );
	}

	if ( cf_str_pwd != NULL )
	{
		CFRelease( cf_str_pwd );
	}

	return( out_result );

} /* od_do_clear_text_auth */


/* -----------------------------------------------------------------
	od_open ()
   ----------------------------------------------------------------- */

bool od_open ( void )
{
	CFErrorRef	cf_err_ref;

	od_session_ref = ODSessionCreate( kCFAllocatorDefault, NULL, &cf_err_ref );
	if ( od_session_ref == NULL )
	{
		/* print the error and bail */
		print_cf_error( cf_err_ref, "Unable to create OD Session" );
		return( FALSE );
	}

	od_node_ref = ODNodeCreateWithNodeType( kCFAllocatorDefault, od_session_ref, kODNodeTypeAuthentication, &cf_err_ref );
	if ( od_session_ref == NULL )
	{
		/* print the error and bail */
		print_cf_error( cf_err_ref, "Unable to create OD Node Reference" );

		/* release OD session */
		CFRelease( od_session_ref );
		od_session_ref = NULL;
		return( FALSE );
	}

	CFRetain( od_session_ref );
	CFRetain( od_node_ref );

	return( TRUE );

} /* od_open */


/* -----------------------------------------------------------------
	od_get_user_record ()
   ----------------------------------------------------------------- */

static ODRecordRef od_get_user_record ( const char *in_user )
{
	CFStringRef				cf_str_user		= NULL;
	ODRecordRef				od_rec_ref		= NULL;
	CFTypeRef				cf_type_val[]	= { CFSTR(kDSAttributesStandardAll) };
	CFArrayRef				cf_arry_attr	= CFArrayCreate( NULL, cf_type_val, 1, &kCFTypeArrayCallBacks );
	CFErrorRef				cf_err_ref		= NULL;

	cf_str_user = CFStringCreateWithCString( NULL, in_user, kCFStringEncodingUTF8 );
	if ( cf_str_user == NULL )
	{
		msg_error( "Unable to create user name CFStringRef" );
		return( NULL );
	}

	od_rec_ref = ODNodeCopyRecord( od_node_ref, CFSTR(kDSStdRecordTypeUsers), cf_str_user, cf_arry_attr, &cf_err_ref );
	if ( od_rec_ref == NULL )
	{
		/* print the error and bail */
		print_cf_error( cf_err_ref, "Unable to lookup user record" );
	}

	if ( cf_str_user != NULL )
	{
		CFRelease( cf_str_user );
	}

	if ( cf_arry_attr != NULL )
	{
		CFRelease( cf_arry_attr );
	}

	return( od_rec_ref );

} /* od_get_user_record */


/* -----------------------------------------------------------------
	od_validate_response ()
   ----------------------------------------------------------------- */

int od_validate_response ( const char *in_user, const char *in_chal, const char *in_resp, const char *in_auth_type )
{
	bool					b_result		= FALSE;
	CFStringRef				cf_str_user		= NULL;
	CFErrorRef				cf_err_ref		= NULL;
	ODRecordRef				od_rec_ref		= NULL;
	CFStringRef				cf_str_chal		= NULL;
	CFStringRef				cf_str_resp		= NULL;
	CFMutableArrayRef		cf_arry_buf		= CFArrayCreateMutable( NULL, 3, &kCFTypeArrayCallBacks );
	CFArrayRef				cf_arry_resp	= NULL;
	ODContextRef			od_context_ref	= NULL;
	ODAuthenticationType	od_auth_type;

	if ( (in_user == NULL) || (in_chal == NULL) || (in_resp == NULL) || (in_auth_type == NULL) )
	{
		msg_error( "AOD: Invalid argument passed to validate response" );
		return( eAOD_param_error );
	}

	if ( od_open() == FALSE )
	{
		return( eAOD_open_OD_failed );
	}

	od_rec_ref = od_get_user_record( in_user );
	if ( od_rec_ref == NULL )
	{
		/* print the error and bail */
		print_cf_error( cf_err_ref, "Unable to lookup user record" );

		/* release OD session */
		return( eAOD_system_error );
	}

	/* Stuff auth buffer with, user/record name, challenge and response  */
	cf_str_user = CFStringCreateWithCString( NULL, in_user, kCFStringEncodingUTF8 );
	CFArrayAppendValue( cf_arry_buf, cf_str_user );

	cf_str_chal = CFStringCreateWithCString( NULL, in_chal, kCFStringEncodingUTF8 );
	CFArrayAppendValue( cf_arry_buf, cf_str_chal );

	cf_str_resp = CFStringCreateWithCString( NULL, in_resp, kCFStringEncodingUTF8 );
	CFArrayAppendValue( cf_arry_buf, cf_str_resp );

	/* Make the "3 AM" call */
	b_result = ODRecordVerifyPasswordExtended( od_rec_ref, kODAuthenticationTypeCRAM_MD5, cf_arry_buf, &cf_arry_resp, &od_context_ref, &cf_err_ref );

	/* do some clean up */
	if ( cf_str_user != NULL )
	{
		CFRelease( cf_str_user );
	}

	if ( cf_err_ref != NULL )
	{
		CFRelease( cf_err_ref );
	}

	if ( od_rec_ref != NULL )
	{
		CFRelease( od_rec_ref );
	}

	if ( cf_str_chal != NULL )
	{
		CFRelease( cf_str_chal );
	}

	if ( cf_str_resp != NULL )
	{
		CFRelease( cf_str_resp );
	}

	if ( cf_arry_buf != NULL )
	{
		CFRelease( cf_arry_buf );
	}

	if ( cf_arry_resp != NULL )
	{
		CFRelease( cf_arry_resp );
	}

	/* Were they ready to receive the "3 AM" call */
	if ( b_result == TRUE )
	{
		return( eAOD_no_error );
	}

	return( eAOD_passwd_mismatch );

} /* od_validate_response */

#endif /* __APPLE_OS_X_SERVER__ */
#endif
