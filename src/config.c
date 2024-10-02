/*
 * Pound - the reverse-proxy load-balancer
 * Copyright (C) 2002-2010 Apsis GmbH
 * Copyright (C) 2018-2024 Sergey Poznyakoff
 *
 * Pound is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Pound is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with pound.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pound.h"
#include "extern.h"
#include "resolver.h"
#include <openssl/x509v3.h>
#include <assert.h>
#include <dirent.h>
#include <sys/stat.h>

static void
regcomp_error_at_locus_range (struct locus_range const *loc, GENPAT rx,
			      char const *expr)
{
  size_t off;
  char const *errmsg = genpat_error (rx, &off);

  if (off)
    conf_error_at_locus_range (loc, "%s at byte %zu", errmsg, off);
  else
    conf_error_at_locus_range (loc, "%s", errmsg);
  if (expr)
    conf_error_at_locus_range (loc, "regular expression: %s", expr);
}

static void
openssl_error_at_locus_range (struct locus_range const *loc,
			      char const *filename, char const *msg)
{
  unsigned long n = ERR_get_error ();
  if (filename)
    conf_error_at_locus_range (loc, "%s: %s: %s", filename, msg,
			       ERR_error_string (n, NULL));
  else
    conf_error_at_locus_range (loc, "%s: %s", msg, ERR_error_string (n, NULL));

  if ((n = ERR_get_error ()) != 0)
    {
      do
	{
	  conf_error_at_locus_range (loc, "%s", ERR_error_string (n, NULL));
	}
      while ((n = ERR_get_error ()) != 0);
    }
}

#define conf_regcomp_error(rc, rx, expr)				\
  regcomp_error_at_locus_range (last_token_locus_range (), rx, expr)

#define conf_openssl_error(file, msg)				\
  openssl_error_at_locus_range (last_token_locus_range (), file, msg)

/*
 * Named backends
 */
typedef struct named_backend
{
  char *name;
  struct locus_range locus;
  int priority;
  int disabled;
  struct be_matrix bemtx;
  SLIST_ENTRY (named_backend) link;
} NAMED_BACKEND;

#define HT_TYPE NAMED_BACKEND
#include "ht.h"

typedef struct named_backend_table
{
  NAMED_BACKEND_HASH *hash;
  SLIST_HEAD(,named_backend) head;
} NAMED_BACKEND_TABLE;

static void
named_backend_table_init (NAMED_BACKEND_TABLE *tab)
{
  tab->hash = NAMED_BACKEND_HASH_NEW ();
  SLIST_INIT (&tab->head);
}

static void
named_backend_table_free (NAMED_BACKEND_TABLE *tab)
{
  NAMED_BACKEND_HASH_FREE (tab->hash);
  while (!SLIST_EMPTY (&tab->head))
    {
      NAMED_BACKEND *ent = SLIST_FIRST (&tab->head);
      SLIST_SHIFT (&tab->head, link);
      free (ent);
    }
}

static NAMED_BACKEND *
named_backend_insert (NAMED_BACKEND_TABLE *tab, char const *name,
		      struct locus_range const *locus,
		      BACKEND *be)
{
  NAMED_BACKEND *bp, *old;

  bp = xmalloc (sizeof (*bp) + strlen (name) + 1);
  bp->name = (char*) (bp + 1);
  strcpy (bp->name, name);
  bp->locus = *locus;
  bp->priority = be->priority;
  bp->disabled = be->disabled;
  bp->bemtx = be->v.mtx;
  if ((old = NAMED_BACKEND_INSERT (tab->hash, bp)) != NULL)
    {
      free (bp);
      return old;
    }
  SLIST_PUSH (&tab->head, bp, link);
  return NULL;
}

static NAMED_BACKEND *
named_backend_retrieve (NAMED_BACKEND_TABLE *tab, char const *name)
{
  NAMED_BACKEND key;

  key.name = (char*) name;
  return NAMED_BACKEND_RETRIEVE (tab->hash, &key);
}

typedef struct
{
  int log_level;
  int facility;
  unsigned clnt_to;
  unsigned be_to;
  unsigned ws_to;
  unsigned be_connto;
  unsigned ignore_case;
  int re_type;
  int header_options;
  BALANCER_ALGO balancer_algo;
  NAMED_BACKEND_TABLE named_backend_table;
  struct resolver_config resolver;
} POUND_DEFAULTS;

/*
 * The ai_flags in the struct addrinfo is not used, unless in hints.
 * Therefore it is reused to mark which parts of address have been
 * initialized.
 */
#define ADDRINFO_SET_ADDRESS(addr) ((addr)->ai_flags = AI_NUMERICHOST)
#define ADDRINFO_HAS_ADDRESS(addr) ((addr)->ai_flags & AI_NUMERICHOST)
#define ADDRINFO_SET_PORT(addr) ((addr)->ai_flags |= AI_NUMERICSERV)
#define ADDRINFO_HAS_PORT(addr) ((addr)->ai_flags & AI_NUMERICSERV)

static int
resolve_address (char const *node, struct locus_range *locus, int family,
		 struct addrinfo *addr)
{
  if (get_host (node, addr, family))
    {
      /* if we can't resolve it assume this is a UNIX domain socket */
      struct sockaddr_un *sun;
      size_t len = strlen (node);
      if (len > UNIX_PATH_MAX)
	{
	  conf_error_at_locus_range (locus, "%s", "UNIX path name too long");
	  return CFGPARSER_FAIL;
	}

      len += offsetof (struct sockaddr_un, sun_path) + 1;
      sun = xmalloc (len);
      sun->sun_family = AF_UNIX;
      strcpy (sun->sun_path, node);

      addr->ai_socktype = SOCK_STREAM;
      addr->ai_family = AF_UNIX;
      addr->ai_protocol = 0;
      addr->ai_addr = (struct sockaddr *) sun;
      addr->ai_addrlen = len;
    }
  return CFGPARSER_OK;
}

static int
assign_address_internal (struct addrinfo *addr, struct token *tok)
{
  int res;

  if (!tok)
    return CFGPARSER_FAIL;

  if (tok->type != T_IDENT && tok->type != T_LITERAL && tok->type != T_STRING)
    {
      conf_error_at_locus_range (&tok->locus,
				 "expected hostname or IP address, but found %s",
				 token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }

  res = resolve_address (tok->str, &tok->locus, AF_UNSPEC, addr);
  if (res == CFGPARSER_OK)
    ADDRINFO_SET_ADDRESS (addr);
  return res;
}

static int
assign_address_string (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect_mask (T_BIT (T_IDENT) |
					  T_BIT (T_STRING) |
					  T_BIT (T_LITERAL));
  if (!tok)
    return CFGPARSER_FAIL;
  *(char**)call_data = xstrdup (tok->str);
  return CFGPARSER_OK;
}

static int
assign_address (void *call_data, void *section_data)
{
  struct addrinfo *addr = call_data;

  if (ADDRINFO_HAS_ADDRESS (addr))
    {
      conf_error ("%s", "Duplicate Address statement");
      return CFGPARSER_FAIL;
    }

  return assign_address_internal (call_data, gettkn_any ());
}

static int
assign_address_family (void *call_data, void *section_data)
{
  static struct kwtab kwtab[] = {
    { "any",  AF_UNSPEC },
    { "unix", AF_UNIX },
    { "inet", AF_INET },
    { "inet6", AF_INET6 },
    { NULL }
  };
  return cfg_assign_int_enum (call_data, gettkn_expect (T_IDENT), kwtab,
			      "address family name");
}

static int
assign_port_generic (struct token *tok, int family, int *port)
{
  struct addrinfo hints, *res;
  int rc;

  if (!tok)
    return CFGPARSER_FAIL;

  if (tok->type != T_IDENT && tok->type != T_NUMBER)
    {
      conf_error_at_locus_range (&tok->locus,
				 "expected port number or service name, but found %s",
				 token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }

  memset (&hints, 0, sizeof(hints));
  hints.ai_flags = 0;
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  rc = getaddrinfo (NULL, tok->str, &hints, &res);
  if (rc != 0)
    {
      conf_error_at_locus_range (&tok->locus,
				 "bad port number: %s", gai_strerror (rc));
      return CFGPARSER_FAIL;
    }

  switch (res->ai_family)
    {
    case AF_INET:
      *port = ((struct sockaddr_in *)res->ai_addr)->sin_port;
      break;

    case AF_INET6:
      *port = ((struct sockaddr_in6 *)res->ai_addr)->sin6_port;
      break;

    default:
      conf_error_at_locus_range (&tok->locus, "%s",
				 "Port is supported only for INET/INET6 back-ends");
      return CFGPARSER_FAIL;
    }
  freeaddrinfo (res);
  return CFGPARSER_OK;
}

static int
assign_port_internal (struct addrinfo *addr, struct token *tok)
{
  int port;
  int res = assign_port_generic (tok, addr->ai_family, &port);

  if (res == CFGPARSER_OK)
    {
      switch (addr->ai_family)
	{
	case AF_INET:
	  ((struct sockaddr_in *)addr->ai_addr)->sin_port = port;
	  break;

	case AF_INET6:
	  ((struct sockaddr_in6 *)addr->ai_addr)->sin6_port = port;
	  break;

	default:
	  // should not happen: handled by assign_port_generic
	  abort ();
	}
      ADDRINFO_SET_PORT (addr);
    }
  return CFGPARSER_OK;
}

static int
assign_port_addrinfo (void *call_data, void *section_data)
{
  struct addrinfo *addr = call_data;

  if (ADDRINFO_HAS_PORT (addr))
    {
      conf_error ("%s", "Duplicate port statement");
      return CFGPARSER_FAIL;
    }
  if (!(ADDRINFO_HAS_ADDRESS (addr)))
    {
      conf_error ("%s", "Address statement should precede Port");
      return CFGPARSER_FAIL;
    }

  return assign_port_internal (call_data, gettkn_any ());
}

static int
assign_port_int (void *call_data, void *section_data)
{
  return assign_port_generic (gettkn_any (), AF_UNSPEC, call_data);
}

static int
assign_CONTENT_LENGTH (void *call_data, void *section_data)
{
  CONTENT_LENGTH n;
  char *p;
  struct token *tok = gettkn_expect (T_NUMBER);

  if (!tok)
    return CFGPARSER_FAIL;

  if (strtoclen (tok->str, 10, &n, &p) || *p)
    {
      conf_error ("%s", "bad long number");
      return CFGPARSER_FAIL;
    }
  *(CONTENT_LENGTH *)call_data = n;
  return 0;
}  

/*
 * ACL support
 */

/* Max. number of bytes in an inet address (suitable for both v4 and v6) */
#define MAX_INADDR_BYTES 16

typedef struct cidr
{
  int family;                           /* Address family */
  int len;                              /* Address length */
  unsigned char addr[MAX_INADDR_BYTES]; /* Network address */
  unsigned char mask[MAX_INADDR_BYTES]; /* Address mask */
  SLIST_ENTRY (cidr) next;              /* Link to next CIDR */
} CIDR;

/* Create a new ACL. */
static ACL *
new_acl (char const *name)
{
  ACL *acl;

  XZALLOC (acl);
  if (name)
    acl->name = xstrdup (name);
  else
    acl->name = NULL;
  SLIST_INIT (&acl->head);

  return acl;
}

/* Match cidr against inet address ap/len.  Return 0 on match, 1 otherwise. */
static int
cidr_match (CIDR *cidr, unsigned char *ap, size_t len)
{
  size_t i;

  if (cidr->len == len)
    {
      for (i = 0; i < len; i++)
	{
	  if (cidr->addr[i] != (ap[i] & cidr->mask[i]))
	    return 1;
	}
    }
  return 0;
}

/*
 * Split the inet address of SA to address pointer and length, suitable
 * for use with the above functions.  Store pointer in RET_PTR.  Return
 * address length in bytes, or -1 if SA has invalid address family.
 */
int
sockaddr_bytes (struct sockaddr *sa, unsigned char **ret_ptr)
{
  switch (sa->sa_family)
    {
    case AF_INET:
      *ret_ptr = (unsigned char *) &(((struct sockaddr_in*)sa)->sin_addr.s_addr);
      return 4;

    case AF_INET6:
      *ret_ptr = (unsigned char *) &(((struct sockaddr_in6*)sa)->sin6_addr);
      return 16;

    default:
      break;
    }
  return -1;
}

/*
 * Match sockaddr SA against ACL.  Return 0 if it matches, 1 if it does not
 * and -1 on error (invalid address family).
 */
int
acl_match (ACL *acl, struct sockaddr *sa)
{
  CIDR *cidr;
  unsigned char *ap;
  size_t len;

  if ((len = sockaddr_bytes (sa, &ap)) == -1)
    return -1;

  SLIST_FOREACH (cidr, &acl->head, next)
    {
      if (cidr->family == sa->sa_family && cidr_match (cidr, ap, len) == 0)
	return 0;
    }

  return 1;
}

static void
masklen_to_netmask (unsigned char *buf, size_t len, size_t masklen)
{
  int i, cnt;

  cnt = masklen / 8;
  for (i = 0; i < cnt; i++)
    buf[i] = 0xff;
  if (i == MAX_INADDR_BYTES)
    return;
  cnt = 8 - masklen % 8;
  buf[i++] = (0xff >> cnt) << cnt;
  for (; i < MAX_INADDR_BYTES; i++)
    buf[i] = 0;
}

/* Parse CIDR at the current point of the input. */
static int
parse_cidr (ACL *acl)
{
  struct token *tok;
  char *mask;
  struct addrinfo hints, *res;
  unsigned long masklen;
  int rc;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  if ((mask = strchr (tok->str, '/')) != NULL)
    {
      char *p;

      *mask++ = 0;

      errno = 0;
      masklen = strtoul (mask, &p, 10);
      if (errno || *p)
	{
	  conf_error ("%s", "invalid netmask");
	  return CFGPARSER_FAIL;
	}
    }

  memset (&hints, 0, sizeof (hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = AI_NUMERICHOST;

  if ((rc = getaddrinfo (tok->str, NULL, &hints, &res)) == 0)
    {
      CIDR *cidr;
      int len, i;
      unsigned char *p;

      if ((len = sockaddr_bytes (res->ai_addr, &p)) == -1)
	{
	  conf_error ("%s", "unsupported address family");
	  return CFGPARSER_FAIL;
	}
      XZALLOC (cidr);
      cidr->family = res->ai_family;
      cidr->len = len;
      memcpy (cidr->addr, p, len);
      if (!mask)
	masklen = len * 8;
      masklen_to_netmask (cidr->mask, cidr->len, masklen);
      /* Fix-up network address, just in case */
      for (i = 0; i < len; i++)
	cidr->addr[i] &= cidr->mask[i];
      SLIST_PUSH (&acl->head, cidr, next);
      freeaddrinfo (res);
    }
  else
    {
      conf_error ("%s", "invalid IP address: %s", gai_strerror (rc));
      return CFGPARSER_FAIL;
    }
  return CFGPARSER_OK;
}

/*
 * List of named ACLs.
 * There shouldn't be many of them, so it's perhaps no use in implementing
 * more sophisticated data structures than a mere singly-linked list.
 */
static ACL_HEAD acl_list = SLIST_HEAD_INITIALIZER (acl_list);

/*
 * Return a pointer to the named ACL, or NULL if no ACL with such name is
 * found.
 */
static ACL *
acl_by_name (char const *name)
{
  ACL *acl;
  SLIST_FOREACH (acl, &acl_list, next)
    {
      if (strcmp (acl->name, name) == 0)
	break;
    }
  return acl;
}

/*
 * Parse ACL definition.
 * On entry, input must be positioned on the next token after ACL ["name"].
 */
static int
parse_acl (ACL *acl)
{
  struct token *tok;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;

  if (tok->type != '\n')
    {
      conf_error ("expected newline, but found %s", token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }

  for (;;)
    {
      int rc;
      if ((tok = gettkn_any ()) == NULL)
	return CFGPARSER_FAIL;
      if (tok->type == '\n')
	continue;
      if (tok->type == T_IDENT)
	{
	  if (strcasecmp (tok->str, "end") == 0)
	    break;
	  if (strcasecmp (tok->str, "include") == 0)
	    {
	      if ((rc = cfg_parse_include (NULL, NULL)) == CFGPARSER_FAIL)
		return rc;
	      continue;
	    }
	  conf_error ("expected CIDR, \"Include\", or \"End\", but found %s",
		      token_type_str (tok->type));
	  return CFGPARSER_FAIL;
	}
      putback_tkn (tok);
      if ((rc = parse_cidr (acl)) != CFGPARSER_OK)
	return rc;
    }
  return CFGPARSER_OK;
}

/*
 * Parse a named ACL.
 * Input is positioned after the "ACL" keyword.
 */
static int
parse_named_acl (void *call_data, void *section_data)
{
  ACL *acl;
  struct token *tok;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  if (acl_by_name (tok->str))
    {
      conf_error ("%s", "ACL with that name already defined");
      return CFGPARSER_FAIL;
    }

  acl = new_acl (tok->str);
  SLIST_PUSH (&acl_list, acl, next);

  return parse_acl (acl);
}

/*
 * Parse ACL reference.  Two forms are accepted:
 * ACL "name"
 *   References a named ACL.
 * ACL "\n" ... End
 *   Creates and references an unnamed ACL.
 */
static int
parse_acl_ref (ACL **ret_acl)
{
  struct token *tok;
  ACL *acl;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;

  if (tok->type == '\n')
    {
      putback_tkn (tok);
      acl = new_acl (NULL);
      *ret_acl = acl;
      return parse_acl (acl);
    }
  else if (tok->type == T_STRING)
    {
      if ((acl = acl_by_name (tok->str)) == NULL)
	{
	  conf_error ("no such ACL: %s", tok->str);
	  return CFGPARSER_FAIL;
	}
      *ret_acl = acl;
    }
  else
    {
      conf_error ("expected ACL name or definition, but found %s",
		  token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }
  return CFGPARSER_OK;
}

static int
assign_acl (void *call_data, void *section_data)
{
  return parse_acl_ref (call_data);
}

static int
parse_ECDHCurve (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return CFGPARSER_FAIL;
#if SET_DH_AUTO == 0 && !defined OPENSSL_NO_ECDH
  if (set_ECDHCurve (tok->str) == 0)
    {
      conf_error ("%s", "ECDHCurve: invalid curve name");
      return CFGPARSER_FAIL;
    }
#else
  conf_error ("%s", "statement ignored");
#endif
  return CFGPARSER_OK;
}

static int
parse_SSLEngine (void *call_data, void *section_data)
{
  struct token *tok = gettkn_expect (T_STRING);
  if (!tok)
    return CFGPARSER_FAIL;
#if HAVE_OPENSSL_ENGINE_H && OPENSSL_VERSION_MAJOR < 3
  ENGINE *e;

  if (!(e = ENGINE_by_id (tok->str)))
    {
      conf_error ("%s", "unrecognized engine");
      return CFGPARSER_FAIL;
    }

  if (!ENGINE_init (e))
    {
      ENGINE_free (e);
      conf_error ("%s", "could not init engine");
      return CFGPARSER_FAIL;
    }

  if (!ENGINE_set_default (e, ENGINE_METHOD_ALL))
    {
      ENGINE_free (e);
      conf_error ("%s", "could not set all defaults");
    }

  ENGINE_finish (e);
  ENGINE_free (e);
#else
  conf_error ("%s", "statement ignored");
#endif

  return CFGPARSER_OK;
}

static int
backend_parse_https (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct stringbuf sb;

  if ((be->v.mtx.ctx = SSL_CTX_new (SSLv23_client_method ())) == NULL)
    {
      conf_openssl_error (NULL, "SSL_CTX_new");
      return CFGPARSER_FAIL;
    }

  SSL_CTX_set_app_data (be->v.mtx.ctx, be);
  SSL_CTX_set_verify (be->v.mtx.ctx, SSL_VERIFY_NONE, NULL);
  SSL_CTX_set_mode (be->v.mtx.ctx, SSL_MODE_AUTO_RETRY);
#ifdef SSL_MODE_SEND_FALLBACK_SCSV
  SSL_CTX_set_mode (be->v.mtx.ctx, SSL_MODE_SEND_FALLBACK_SCSV);
#endif
  SSL_CTX_set_options (be->v.mtx.ctx, SSL_OP_ALL);
#ifdef  SSL_OP_NO_COMPRESSION
  SSL_CTX_set_options (be->v.mtx.ctx, SSL_OP_NO_COMPRESSION);
#endif
  SSL_CTX_clear_options (be->v.mtx.ctx,
			 SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
  SSL_CTX_clear_options (be->v.mtx.ctx, SSL_OP_LEGACY_SERVER_CONNECT);

  xstringbuf_init (&sb);
  stringbuf_printf (&sb, "%d-Pound-%ld", getpid (), random ());
  SSL_CTX_set_session_id_context (be->v.mtx.ctx,
				  (unsigned char *) stringbuf_value (&sb),
				  stringbuf_len (&sb));
  stringbuf_free (&sb);

  POUND_SSL_CTX_init (be->v.mtx.ctx);

  return CFGPARSER_OK;
}

static int
backend_parse_cert (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct token *tok;

  if (be->v.mtx.ctx == NULL)
    {
      conf_error ("%s", "HTTPS must be used before this statement");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  if (SSL_CTX_use_certificate_chain_file (be->v.mtx.ctx, tok->str) != 1)
    {
      conf_openssl_error (tok->str, "SSL_CTX_use_certificate_chain_file");
      return CFGPARSER_FAIL;
    }

  if (SSL_CTX_use_PrivateKey_file (be->v.mtx.ctx, tok->str, SSL_FILETYPE_PEM) != 1)
    {
      conf_openssl_error (tok->str, "SSL_CTX_use_PrivateKey_file");
      return CFGPARSER_FAIL;
    }

  if (SSL_CTX_check_private_key (be->v.mtx.ctx) != 1)
    {
      conf_openssl_error (tok->str, "SSL_CTX_check_private_key failed");
      return CFGPARSER_FAIL;
    }

  return CFGPARSER_OK;
}

static int
backend_assign_ciphers (void *call_data, void *section_data)
{
  BACKEND *be = call_data;
  struct token *tok;

  if (be->v.mtx.ctx == NULL)
    {
      conf_error ("%s", "HTTPS must be used before this statement");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  SSL_CTX_set_cipher_list (be->v.mtx.ctx, tok->str);
  return CFGPARSER_OK;
}

static int
backend_assign_priority (void *call_data, void *section_data)
{
  return cfg_assign_int_range (call_data, 0, -1);
}

static int
set_proto_opt (int *opt)
{
  int n;

  static struct kwtab kwtab[] = {
    { "SSLv2", SSL_OP_NO_SSLv2 },
    { "SSLv3", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 },
#ifdef SSL_OP_NO_TLSv1
    { "TLSv1", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_TLSv1 },
#endif
#ifdef SSL_OP_NO_TLSv1_1
    { "TLSv1_1", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
		 SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 },
#endif
#ifdef SSL_OP_NO_TLSv1_2
    { "TLSv1_2", SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 |
		 SSL_OP_NO_TLSv1 | SSL_OP_NO_TLSv1_1 |
		 SSL_OP_NO_TLSv1_2 },
#endif
    { NULL }
  };
  int res = cfg_assign_int_enum (&n, gettkn_expect (T_IDENT), kwtab,
				 "protocol name");
  if (res == CFGPARSER_OK)
    *opt |= n;

  return res;
}

static int
disable_proto (void *call_data, void *section_data)
{
  SSL_CTX *ctx = *(SSL_CTX**) call_data;
  int n = 0;

  if (ctx == NULL)
    {
      conf_error ("%s", "HTTPS must be used before this statement");
      return CFGPARSER_FAIL;
    }

  if (set_proto_opt (&n) != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  SSL_CTX_set_options (ctx, n);

  return CFGPARSER_OK;
}

static struct kwtab resolve_mode_kwtab[] = {
  { "immediate", bres_immediate },
  { "first", bres_first },
  { "all", bres_all },
  { "srv", bres_srv },
  { NULL }
};

char const *
resolve_mode_str (int mode)
{
  char const *ret = kw_to_str (resolve_mode_kwtab, mode);
  return ret ? ret : "UNKNOWN";
}

static int
assign_resolve_mode (void *call_data, void *section_data)
{
  int res = cfg_assign_int_enum (call_data, gettkn_expect (T_IDENT),
				 resolve_mode_kwtab,
				 "backend resolve mode");
#ifndef ENABLE_DYNAMIC_BACKENDS
  if (res != bres_immediate)
    {
      conf_error ("%s", "value not supported: pound compiled without support for dynamic backends");
      res = CFGPARSER_FAIL;
    }
#endif
  return res;
}

static CFGPARSER_TABLE backend_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "Address",
    .parser = assign_address_string,
    .off = offsetof (BACKEND, v.mtx.hostname)
  },
  {
    .name = "Port",
    .parser = assign_port_int,
    .off = offsetof (BACKEND, v.mtx.port)
  },
  {
    .name = "Family",
    .parser = assign_address_family,
    .off = offsetof (BACKEND, v.mtx.family)
  },
  {
    .name = "Resolve",
    .parser = assign_resolve_mode,
    .off = offsetof (BACKEND, v.mtx.resolve_mode)
  },
  {
    .name = "RetryInterval",
    .parser = cfg_assign_timeout,
    .off = offsetof (BACKEND, v.mtx.retry_interval)
  },
  {
    .name = "Priority",
    .parser = backend_assign_priority,
    .off = offsetof (BACKEND, priority)
  },
  {
    .name = "TimeOut",
    .parser = cfg_assign_timeout,
    .off = offsetof (BACKEND, v.mtx.to)
  },
  {
    .name = "WSTimeOut",
    .parser = cfg_assign_timeout,
    .off = offsetof (BACKEND, v.mtx.ws_to)
  },
  {
    .name = "ConnTO",
    .parser = cfg_assign_timeout,
    .off = offsetof (BACKEND, v.mtx.conn_to)
  },
  {
    .name = "HTTPS",
    .parser = backend_parse_https
  },
  {
    .name = "Cert",
    .parser = backend_parse_cert
  },
  {
    .name = "Ciphers",
    .parser = backend_assign_ciphers
  },
  {
    .name = "Disable",
    .parser = disable_proto,
    .off = offsetof (BACKEND, v.mtx.ctx)
  },
  {
    .name = "Disabled",
    .parser = cfg_assign_bool,
    .off = offsetof (BACKEND, disabled)
  },
  {
    .name = "ServerName",
    .parser = cfg_assign_string,
    .off = offsetof (BACKEND, v.mtx.servername)
  },
  { NULL }
};

static CFGPARSER_TABLE use_backend_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "Priority",
    .parser = backend_assign_priority,
    .off = offsetof (BACKEND, priority)
  },
  {
    .name = "Disabled",
    .parser = cfg_assign_bool,
    .off = offsetof (BACKEND, disabled)
  },
  { NULL }
};

static int
check_addrinfo (struct addrinfo const *addr,
		struct locus_range const *range, char const *name)
{
  if (ADDRINFO_HAS_ADDRESS (addr))
    {
      if (!ADDRINFO_HAS_PORT (addr) &&
	  (addr->ai_family == AF_INET || addr->ai_family == AF_INET6))
	{
	  conf_error_at_locus_range (range, "%s missing Port declaration",
				     name);
	  return CFGPARSER_FAIL;
	}
    }
  else
    {
      conf_error_at_locus_range (range, "%s missing Address declaration",
				 name);
      return CFGPARSER_FAIL;
    }
  return CFGPARSER_OK;
}

static char *
format_locus_str (struct locus_range *rp)
{
  struct stringbuf sb;

  xstringbuf_init (&sb);
  stringbuf_format_locus_range (&sb, rp);
  return stringbuf_finish (&sb);
}

static inline int
parser_loop (CFGPARSER_TABLE *ptab,
	     void *call_data, void *section_data,
	     struct locus_range *retrange)
{
  return cfgparser_loop (ptab, call_data, section_data,
			 feature_is_set (FEATURE_WARN_DEPRECATED)
			   ? DEPREC_WARN : DEPREC_OK,
			 retrange);
}

static BACKEND *
parse_backend_internal (CFGPARSER_TABLE *table, POUND_DEFAULTS *dfl,
			struct locus_point *beg)
{
  BACKEND *be;
  struct locus_range range;

  XZALLOC (be);
  be->be_type = BE_MATRIX;
  be->v.mtx.to = dfl->be_to;
  be->v.mtx.conn_to = dfl->be_connto;
  be->v.mtx.ws_to = dfl->ws_to;
  be->priority = 5;
  pthread_mutex_init (&be->mut, NULL);

  if (parser_loop (table, be, dfl, &range))
    return NULL;
  if (beg)
    range.beg = *beg;
  be->locus = range;
  be->locus_str = format_locus_str (&range);

  return be;
}

static int
parse_backend (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  BACKEND *be;
  struct token *tok;
  struct locus_point beg = last_token_locus_range ()->beg;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;

  if (tok->type == T_STRING)
    {
      struct locus_range range;

      range.beg = beg;

      XZALLOC (be);
      be->be_type = BE_BACKEND_REF;
      be->v.be_name = xstrdup (tok->str);
      be->priority = -1;
      be->disabled = -1;
      pthread_mutex_init (&be->mut, NULL);

      if (parser_loop (use_backend_parsetab, be, section_data, &range))
	return CFGPARSER_FAIL;
      be->locus_str = format_locus_str (&tok->locus);
    }
  else
    {
      putback_tkn (tok);
      be = parse_backend_internal (backend_parsetab, section_data, &beg);
      if (!be)
	return CFGPARSER_FAIL;
    }

  balancer_add_backend (balancer_list_get_normal (bml), be);

  return CFGPARSER_OK;
}

static int
parse_use_backend (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  BACKEND *be;
  struct token *tok;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  XZALLOC (be);
  be->be_type = BE_BACKEND_REF;
  be->v.be_name = xstrdup (tok->str);
  be->locus = tok->locus;
  be->locus_str = format_locus_str (&tok->locus);
  be->priority = 5;
  pthread_mutex_init (&be->mut, NULL);

  balancer_add_backend (balancer_list_get_normal (bml), be);

  return CFGPARSER_OK;
}

static int
parse_emergency (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  BACKEND *be;
  POUND_DEFAULTS dfl = *(POUND_DEFAULTS*)section_data;

  dfl.be_to = 120;
  dfl.be_connto = 120;
  dfl.ws_to = 120;

  be = parse_backend_internal (backend_parsetab, &dfl, NULL);
  if (!be)
    return CFGPARSER_FAIL;

  balancer_add_backend (balancer_list_get_emerg (bml), be);

  return CFGPARSER_OK;
}

static int
parse_control_backend (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  BACKEND *be;

  XZALLOC (be);
  be->be_type = BE_CONTROL;
  be->priority = 1;
  pthread_mutex_init (&be->mut, NULL);
  balancer_add_backend (balancer_list_get_normal (bml), be);
  return CFGPARSER_OK;
}

static int
parse_metrics (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  BACKEND *be;

  XZALLOC (be);
  be->be_type = BE_METRICS;
  be->priority = 1;
  pthread_mutex_init (&be->mut, NULL);
  balancer_add_backend (balancer_list_get_normal (bml), be);
  return CFGPARSER_OK;
}

static SERVICE_COND *
service_cond_append (SERVICE_COND *cond, int type)
{
  SERVICE_COND *sc;

  assert (cond->type == COND_BOOL);
  XZALLOC (sc);
  service_cond_init (sc, type);
  SLIST_PUSH (&cond->bool.head, sc, next);

  return sc;
}

static void
stringbuf_escape_regex (struct stringbuf *sb, char const *p)
{
  while (*p)
    {
      size_t len = strcspn (p, "\\[]{}().*+?");
      if (len > 0)
	stringbuf_add (sb, p, len);
      p += len;
      if (*p)
	{
	  stringbuf_add_char (sb, '\\');
	  stringbuf_add_char (sb, *p);
	  p++;
	}
    }
}

static int
parse_match_mode (int dfl_re_type, int *gp_type, int *sp_flags, int *from_file)
{
  struct token *tok;

  enum
  {
    MATCH_RE,
    MATCH_EXACT,
    MATCH_BEG,
    MATCH_END,
    MATCH_CONTAIN,
    MATCH_ICASE,
    MATCH_CASE,
    MATCH_FILE,
    MATCH_POSIX,
    MATCH_PCRE,
  };

  static struct kwtab optab[] = {
    { "-re",      MATCH_RE },
    { "-exact",   MATCH_EXACT },
    { "-beg",     MATCH_BEG },
    { "-end",     MATCH_END },
    { "-contain", MATCH_CONTAIN },
    { "-icase",   MATCH_ICASE },
    { "-case",    MATCH_CASE },
    { "-file",    MATCH_FILE },
    { "-posix",   MATCH_POSIX },
    { "-pcre",    MATCH_PCRE },
    { "-perl",    MATCH_PCRE },
    { NULL }
  };

  if (from_file)
    *from_file = 0;

  for (;;)
    {
      int n;

      if ((tok = gettkn_expect_mask (T_BIT (T_STRING) | T_BIT (T_LITERAL))) == NULL)
	return CFGPARSER_FAIL;

      if (tok->type == T_STRING)
	break;

      if (kw_to_tok (optab, tok->str, 0, &n))
	{
	  conf_error ("unexpected token: %s", tok->str);
	  return CFGPARSER_FAIL;
	}

      switch (n)
	{
	case MATCH_CASE:
	  *sp_flags &= ~GENPAT_ICASE;
	  break;

	case MATCH_ICASE:
	  *sp_flags |= GENPAT_ICASE;
	  break;

	case MATCH_FILE:
	  if (from_file)
	    *from_file = 1;
	  else
	    {
	      conf_error ("unexpected token: %s", tok->str);
	      return CFGPARSER_FAIL;
	    }
	  break;

	case MATCH_RE:
	  *gp_type = dfl_re_type;
	  break;

	case MATCH_POSIX:
	  *gp_type = GENPAT_POSIX;
	  break;

	case MATCH_EXACT:
	  *gp_type = GENPAT_EXACT;
	  break;

	case MATCH_BEG:
	  *gp_type = GENPAT_PREFIX;
	  break;

	case MATCH_END:
	  *gp_type = GENPAT_SUFFIX;
	  break;

	case MATCH_CONTAIN:
	  *gp_type = GENPAT_CONTAIN;
	  break;

	case MATCH_PCRE:
#ifdef HAVE_LIBPCRE
	  *gp_type = GENPAT_PCRE;
#else
	  conf_error ("%s", "pound compiled without PCRE");
	  return CFGPARSER_FAIL;
#endif
	  break;
	}
    }
  putback_tkn (tok);
  return CFGPARSER_OK;
}

static char *
host_prefix_regex (struct stringbuf *sb, int *gp_type, char const *expr)
{
  stringbuf_add_char (sb, '^');
  stringbuf_add_string (sb, "Host:");
  switch (*gp_type)
    {
    case GENPAT_POSIX:
      stringbuf_add_string (sb, "[[:space:]]*");
      if (expr[0] == '^')
	expr++;
      stringbuf_add_string (sb, expr);
      break;

    case GENPAT_PCRE:
      stringbuf_add_string (sb, "\\s*");
      if (expr[0] == '^')
	expr++;
      stringbuf_add_string (sb, expr);
      break;

    case GENPAT_EXACT:
    case GENPAT_PREFIX:
      stringbuf_add_string (sb, "[[:space:]]*");
      stringbuf_escape_regex (sb, expr);
      *gp_type = GENPAT_POSIX;
      break;

    case GENPAT_SUFFIX:
      stringbuf_add_string (sb, "[[:space:]]*");
      stringbuf_add_string (sb, ".*");
      stringbuf_escape_regex (sb, expr);
      stringbuf_add_char (sb, '$');
      *gp_type = GENPAT_POSIX;
      break;

    case GENPAT_CONTAIN:
      stringbuf_add_string (sb, "[[:space:]]*");
      stringbuf_add_string (sb, ".*");
      stringbuf_escape_regex (sb, expr);
      *gp_type = GENPAT_POSIX;
      break;

    default:
      abort ();
    }
  return stringbuf_finish (sb);
}

static int
parse_regex_compat (GENPAT *regex, int dfl_re_type, int gp_type, int flags)
{
  struct token *tok;
  int rc;

  if (parse_match_mode (dfl_re_type, &gp_type, &flags, NULL))
    return CFGPARSER_FAIL;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  rc = genpat_compile (regex, gp_type, tok->str, flags);
  if (rc)
    {
      conf_regcomp_error (rc, *regex, NULL);
      genpat_free (*regex);
      return CFGPARSER_FAIL;
    }

  return CFGPARSER_OK;
}

STRING_REF *
string_ref_alloc (char const *str)
{
  STRING_REF *ref = xmalloc (sizeof (*ref) + strlen (str));
  ref->refcount = 1;
  strcpy (ref->value, str);
  return ref;
}

STRING_REF *
string_ref_incr (STRING_REF *ref)
{
  if (ref)
    ref->refcount++;
  return ref;
}

void
string_ref_free (STRING_REF *ref)
{
  if (ref && --ref->refcount == 0)
    free (ref);
}

static int
parse_cond_matcher_0 (SERVICE_COND *top_cond,
		      enum service_cond_type type,
		      int dfl_re_type,
		      int gp_type, int flags, char const *string)
{
  struct token *tok;
  int rc;
  struct stringbuf sb;
  SERVICE_COND *cond;
  int from_file;
  char *expr;

  if (parse_match_mode (dfl_re_type, &gp_type, &flags, &from_file))
    return CFGPARSER_FAIL;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  xstringbuf_init (&sb);
  if (from_file)
    {
      FILE *fp;
      char *p;
      char buf[MAXBUF];
      STRING_REF *ref = NULL;

      if ((fp = fopen_include (tok->str)) == NULL)
	{
	  fopen_error (LOG_ERR, errno, include_wd, tok->str, &tok->locus);
	  return CFGPARSER_FAIL;
	}

      cond = service_cond_append (top_cond, COND_BOOL);
      cond->bool.op = BOOL_OR;

      switch (type)
	{
	case COND_QUERY_PARAM:
	case COND_STRING_MATCH:
	  ref = string_ref_alloc (string);
	  break;
	default:
	  break;
	}

      while ((p = fgets (buf, sizeof buf, fp)) != NULL)
	{
	  int rc;
	  size_t len;
	  SERVICE_COND *hc;

	  p += strspn (p, " \t");
	  for (len = strlen (p);
	       len > 0 && (p[len-1] == ' ' || p[len-1] == '\t'|| p[len-1] == '\n'); len--)
	    ;
	  if (len == 0 || *p == '#')
	    continue;
	  p[len] = 0;

	  if (type == COND_HOST)
	    {
	      stringbuf_reset (&sb);
	      expr = host_prefix_regex (&sb, &gp_type, p);
	    }
	  else
	    expr = p;

	  hc = service_cond_append (cond, type);
	  rc = genpat_compile (&hc->re, gp_type, expr, flags);
	  if (rc)
	    {
	      conf_regcomp_error (rc, hc->re, NULL);
	      // FIXME: genpat_free (hc->re);
	      return CFGPARSER_FAIL;
	    }
	  switch (type)
	    {
	    case COND_QUERY_PARAM:
	    case COND_STRING_MATCH:
	      memmove (&hc->sm.re, &hc->re, sizeof (hc->sm.re));
	      hc->sm.string = string_ref_incr (ref);
	      break;

	    default:
	      break;
	    }
	}
      string_ref_free (ref);
      fclose (fp);
    }
  else
    {
      cond = service_cond_append (top_cond, type);
      if (type == COND_HOST)
	expr = host_prefix_regex (&sb, &gp_type, tok->str);
      else
	expr = tok->str;
      rc = genpat_compile (&cond->re, gp_type, expr, flags);
      if (rc)
	{
	  conf_regcomp_error (rc, cond->re, NULL);
	  // FIXME: genpat_free (cond->re);
	  return CFGPARSER_FAIL;
	}
      switch (type)
	{
	case COND_QUERY_PARAM:
	case COND_STRING_MATCH:
	  memmove (&cond->sm.re, &cond->re, sizeof (cond->sm.re));
	  cond->sm.string = string_ref_alloc (string);
	  break;

	default:
	  break;
	}
    }
  stringbuf_free (&sb);

  return CFGPARSER_OK;
}

static int
parse_cond_matcher (SERVICE_COND *top_cond,
		    enum service_cond_type type,
		    int dfl_re_type,
		    int gp_type, int flags, char const *string)
{
  int rc;
  char *string_copy;
  if (string)
    string_copy = xstrdup (string);
  else
    string_copy = NULL;
  rc = parse_cond_matcher_0 (top_cond, type, dfl_re_type, gp_type, flags,
			     string_copy);
  free (string_copy);
  return rc;
}

static int
parse_cond_acl (void *call_data, void *section_data)
{
  SERVICE_COND *cond = service_cond_append (call_data, COND_ACL);
  return parse_acl_ref (&cond->acl);
}

static int
parse_cond_url_matcher (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  return parse_cond_matcher (call_data, COND_URL, dfl->re_type,
			     dfl->re_type,
			     (dfl->ignore_case ? GENPAT_ICASE : 0),
			     NULL);
}

static int
parse_cond_path_matcher (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  return parse_cond_matcher (call_data, COND_PATH, dfl->re_type,
			     dfl->re_type,
			     (dfl->ignore_case ? GENPAT_ICASE : 0),
			     NULL);
}

static int
parse_cond_query_matcher (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  return parse_cond_matcher (call_data, COND_QUERY, dfl->re_type,
			     dfl->re_type,
			     (dfl->ignore_case ? GENPAT_ICASE : 0),
			     NULL);
}

static int
parse_cond_query_param_matcher (void *call_data, void *section_data)
{
  SERVICE_COND *top_cond = call_data;
  POUND_DEFAULTS *dfl = section_data;
  int flags = (dfl->ignore_case ? GENPAT_ICASE : 0);
  struct token *tok;
  char *string;
  int rc;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  string = xstrdup (tok->str);
  rc = parse_cond_matcher (top_cond,
			   COND_QUERY_PARAM, dfl->re_type,
			   dfl->re_type, flags, string);
  free (string);
  return rc;
}

static int
parse_cond_string_matcher (void *call_data, void *section_data)
{
  SERVICE_COND *top_cond = call_data;
  POUND_DEFAULTS *dfl = section_data;
  int flags = (dfl->ignore_case ? GENPAT_ICASE : 0);
  struct token *tok;
  char *string;
  int rc;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  string = xstrdup (tok->str);
  rc = parse_cond_matcher (top_cond,
			   COND_STRING_MATCH, dfl->re_type, dfl->re_type, flags,
			   string);
  free (string);
  return rc;
}

static int
parse_cond_hdr_matcher (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  return parse_cond_matcher (call_data, COND_HDR, dfl->re_type, dfl->re_type,
			     GENPAT_MULTILINE | GENPAT_ICASE,
			     NULL);
}

static int
parse_cond_head_deny_matcher (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  SERVICE_COND *cond = service_cond_append (call_data, COND_BOOL);
  cond->bool.op = BOOL_NOT;
  return parse_cond_matcher (cond, COND_HDR, dfl->re_type, dfl->re_type,
			     GENPAT_MULTILINE | GENPAT_ICASE,
			     NULL);
}

static int
parse_cond_host (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  return parse_cond_matcher (call_data, COND_HOST, dfl->re_type,
			     GENPAT_EXACT, GENPAT_ICASE, NULL);
}

static int
parse_cond_basic_auth (void *call_data, void *section_data)
{
  SERVICE_COND *cond = service_cond_append (call_data, COND_BASIC_AUTH);
  struct token *tok;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  cond->pwfile.locus = tok->locus;
  cond->pwfile.filename = xstrdup (tok->str);
  return CFGPARSER_OK;
}

static int
parse_redirect_backend (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  struct token *tok;
  int code = 302;
  BACKEND *be;
  POUND_REGMATCH matches[5];
  struct locus_range range;

  range.beg = last_token_locus_range ()->beg;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;

  if (tok->type == T_NUMBER)
    {
      int n = atoi (tok->str);
      switch (n)
	{
	case 301:
	case 302:
	case 303:
	case 307:
	case 308:
	  code = n;
	  break;

	default:
	  conf_error ("%s", "invalid status code");
	  return CFGPARSER_FAIL;
	}

      if ((tok = gettkn_any ()) == NULL)
	return CFGPARSER_FAIL;
    }

  range.end = last_token_locus_range ()->end;

  if (tok->type != T_STRING)
    {
      conf_error ("expected %s, but found %s", token_type_str (T_STRING), token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }

  XZALLOC (be);
  be->locus_str = format_locus_str (&range);
  be->be_type = BE_REDIRECT;
  be->priority = 1;
  pthread_mutex_init (&be->mut, NULL);

  be->v.redirect.status = code;
  be->v.redirect.url = xstrdup (tok->str);

  if (genpat_match (LOCATION, be->v.redirect.url, 4, matches))
    {
      conf_error ("%s", "Redirect bad URL");
      return CFGPARSER_FAIL;
    }

  if ((be->v.redirect.has_uri = matches[3].rm_eo - matches[3].rm_so) == 1)
    /* the path is a single '/', so remove it */
    be->v.redirect.url[matches[3].rm_so] = '\0';

  balancer_add_backend (balancer_list_get_normal (bml), be);

  return CFGPARSER_OK;
}

static int
parse_error_backend (void *call_data, void *section_data)
{
  BALANCER_LIST *bml = call_data;
  struct token *tok;
  int n, status;
  char *text = NULL;
  BACKEND *be;
  int rc;
  struct locus_range range;

  range.beg = last_token_locus_range ()->beg;

  if ((tok = gettkn_expect (T_NUMBER)) == NULL)
    return CFGPARSER_FAIL;

  n = atoi (tok->str);
  if ((status = http_status_to_pound (n)) == -1)
    {
      conf_error ("%s", "unsupported status code");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;

  if (tok->type == T_STRING)
    {
      putback_tkn (tok);
      if ((rc = cfg_assign_string_from_file (&text, section_data)) == CFGPARSER_FAIL)
	return rc;
    }
  else if (tok->type == '\n')
    rc = CFGPARSER_OK_NONL;
  else
    {
      conf_error ("%s", "string or newline expected");
      return CFGPARSER_FAIL;
    }

  range.end = last_token_locus_range ()->end;

  XZALLOC (be);
  be->locus = range;
  be->locus_str = format_locus_str (&range);
  be->be_type = BE_ERROR;
  be->priority = 1;
  pthread_mutex_init (&be->mut, NULL);

  be->v.error.status = status;
  be->v.error.text = text;

  balancer_add_backend (balancer_list_get_normal (bml), be);

  return rc;
}

static int
parse_errorfile (void *call_data, void *section_data)
{
  struct token *tok;
  int status;
  char **http_err = call_data;

  if ((tok = gettkn_expect (T_NUMBER)) == NULL)
    return CFGPARSER_FAIL;

  if ((status = http_status_to_pound (atoi (tok->str))) == -1)
    {
      conf_error ("%s", "unsupported status code");
      return CFGPARSER_FAIL;
    }

  return cfg_assign_string_from_file (&http_err[status], section_data);
}

struct service_session
{
  int type;
  char *id;
  unsigned ttl;
};

static struct kwtab sess_type_tab[] = {
  { "IP", SESS_IP },
  { "COOKIE", SESS_COOKIE },
  { "URL", SESS_URL },
  { "PARM", SESS_PARM },
  { "BASIC", SESS_BASIC },
  { "HEADER", SESS_HEADER },
  { NULL }
};

char const *
sess_type_to_str (int type)
{
  if (type == SESS_NONE)
    return "NONE";
  return kw_to_str (sess_type_tab, type);
}

static int
session_type_parser (void *call_data, void *section_data)
{
  SERVICE *svc = call_data;
  struct token *tok;
  int n;

  if ((tok = gettkn_expect (T_IDENT)) == NULL)
    return CFGPARSER_FAIL;

  if (kw_to_tok (sess_type_tab, tok->str, 1, &n))
    {
      conf_error ("%s", "Unknown Session type");
      return CFGPARSER_FAIL;
    }
  svc->sess_type = n;

  return CFGPARSER_OK;
}

static CFGPARSER_TABLE session_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "Type",
    .parser = session_type_parser
  },
  {
    .name = "TTL",
    .parser = cfg_assign_timeout,
    .off = offsetof (SERVICE, sess_ttl)
  },
  {
    .name = "ID",
    .parser = cfg_assign_string,
    .off = offsetof (SERVICE, sess_id)
  },
  { NULL }
};

static int
parse_session (void *call_data, void *section_data)
{
  SERVICE *svc = call_data;
  struct locus_range range;

  if (parser_loop (session_parsetab, svc, section_data, &range))
    return CFGPARSER_FAIL;

  if (svc->sess_type == SESS_NONE)
    {
      conf_error_at_locus_range (&range, "Session type not defined");
      return CFGPARSER_FAIL;
    }

  if (svc->sess_ttl == 0)
    {
      conf_error_at_locus_range (&range, "Session TTL not defined");
      return CFGPARSER_FAIL;
    }

  switch (svc->sess_type)
    {
    case SESS_COOKIE:
    case SESS_URL:
    case SESS_HEADER:
      if (svc->sess_id == NULL)
	{
	  conf_error ("%s", "Session ID not defined");
	  return CFGPARSER_FAIL;
	}
      break;

    default:
      break;
    }

  return CFGPARSER_OK;
}

static int
assign_dfl_ignore_case (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  return cfg_assign_bool (&dfl->ignore_case, NULL);
}

static int parse_cond (int op, SERVICE_COND *cond, void *section_data);

static int
parse_match (void *call_data, void *section_data)
{
  struct token *tok;
  int op = BOOL_AND;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;
  if (tok->type == T_IDENT)
    {
      if (strcasecmp (tok->str, "and") == 0)
	op = BOOL_AND;
      else if (strcasecmp (tok->str, "or") == 0)
	op = BOOL_OR;
      else
	{
	  conf_error ("expected AND or OR, but found %s", tok->str);
	  return CFGPARSER_FAIL;
	}
    }
  else
    putback_tkn (tok);

  return parse_cond (op, call_data, section_data);
}

static int parse_not_cond (void *call_data, void *section_data);

static CFGPARSER_TABLE match_conditions[] = {
  {
    .name = "ACL",
    .parser = parse_cond_acl
  },
  {
    .name = "URL",
    .parser = parse_cond_url_matcher
  },
  {
    .name = "Path",
    .parser = parse_cond_path_matcher
  },
  {
    .name = "Query",
    .parser = parse_cond_query_matcher
  },
  {
    .name = "QueryParam",
    .parser = parse_cond_query_param_matcher
  },
  {
    .name = "Header",
    .parser = parse_cond_hdr_matcher
  },
  {
    .name = "HeadRequire",
    .type = KWT_ALIAS,
    .deprecated = 1
  },
  {
    .name = "HeadDeny",
    .parser = parse_cond_head_deny_matcher,
    .deprecated = 1,
    .message = "use \"Not Header\" instead"
  },
  {
    .name = "Host",
    .parser = parse_cond_host
  },
  {
    .name = "BasicAuth",
    .parser = parse_cond_basic_auth
  },
  {
    .name = "StringMatch",
    .parser = parse_cond_string_matcher
  },
  {
    .name = "Match",
    .parser = parse_match
  },
  {
    .name = "NOT",
    .parser = parse_not_cond
  },
  { NULL }
};

static CFGPARSER_TABLE negate_parsetab[] = {
  {
    .name = "",
    .type = KWT_SOFTREF,
    .ref = match_conditions
  },
  { NULL }
};

static int
parse_not_cond (void *call_data, void *section_data)
{
  SERVICE_COND *cond = service_cond_append (call_data, COND_BOOL);
  cond->bool.op = BOOL_NOT;
  return cfgparser (negate_parsetab, cond, section_data,
		    1,
		    feature_is_set (FEATURE_WARN_DEPRECATED)
			   ? DEPREC_WARN : DEPREC_OK,
		    NULL);
}

static CFGPARSER_TABLE logcon_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "",
    .type = KWT_SOFTREF,
    .ref = match_conditions
  },
  { NULL }
};

static int
parse_cond (int op, SERVICE_COND *cond, void *section_data)
{
  SERVICE_COND *subcond = service_cond_append (cond, COND_BOOL);
  struct locus_range range;

  subcond->bool.op = op;
  return parser_loop (logcon_parsetab, subcond, section_data, &range);
}

static int parse_else (void *call_data, void *section_data);
static int parse_rewrite (void *call_data, void *section_data);
static int parse_set_header (void *call_data, void *section_data);
static int parse_delete_header (void *call_data, void *section_data);
static int parse_set_url (void *call_data, void *section_data);
static int parse_set_path (void *call_data, void *section_data);
static int parse_set_query (void *call_data, void *section_data);
static int parse_set_query_param (void *call_data, void *section_data);
static int parse_sub_rewrite (void *call_data, void *section_data);

static CFGPARSER_TABLE rewrite_ops[] = {
  {
    .name = "SetHeader",
    .parser = parse_set_header
  },
  {
    .name = "DeleteHeader",
    .parser = parse_delete_header
  },
  {
    .name = "SetURL",
    .parser = parse_set_url },
  {
    .name = "SetPath",
    .parser = parse_set_path
  },
  {
    .name = "SetQuery",
    .parser = parse_set_query
  },
  {
    .name = "SetQueryParam",
    .parser = parse_set_query_param
  },
  { NULL }
};

static CFGPARSER_TABLE rewrite_rule_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "Rewrite",
    .parser = parse_sub_rewrite,
    .off = offsetof (REWRITE_RULE, ophead)
  },
  {
    .name = "Else",
    .parser = parse_else,
    .off = offsetof (REWRITE_RULE, iffalse)
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, cond),
    .type = KWT_SOFTREF,
    .ref = match_conditions
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, ophead),
    .type = KWT_SOFTREF,
    .ref = rewrite_ops
  },
  { NULL }
};

static int
parse_end_else (void *call_data, void *section_data)
{
  struct token nl = { '\n' };
  putback_tkn (NULL);
  putback_tkn (&nl);
  return CFGPARSER_END;
}

static CFGPARSER_TABLE else_rule_parsetab[] = {
  {
    .name = "End",
    .parser = parse_end_else
  },
  {
    .name = "Rewrite",
    .parser = parse_sub_rewrite,
    .off = offsetof (REWRITE_RULE, ophead)
  },
  {
    .name = "Else",
    .parser = parse_else,
    .off = offsetof (REWRITE_RULE, iffalse)
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, cond),
    .type = KWT_SOFTREF,
    .ref = match_conditions
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, ophead),
    .type = KWT_SOFTREF,
    .ref = rewrite_ops
  },
  { NULL }
};

static REWRITE_OP *
rewrite_op_alloc (REWRITE_OP_HEAD *head, enum rewrite_type type)
{
  REWRITE_OP *op;

  XZALLOC (op);
  op->type = type;
  SLIST_PUSH (head, op, next);

  return op;
}

static int
parse_rewrite_op (REWRITE_OP_HEAD *head, enum rewrite_type type)
{
  REWRITE_OP *op = rewrite_op_alloc (head, type);
  struct token *tok;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  op->v.str = xstrdup (tok->str);
  return CFGPARSER_OK;
}

static int
parse_delete_header (void *call_data, void *section_data)
{
  REWRITE_OP *op = rewrite_op_alloc (call_data, REWRITE_HDR_DEL);
  POUND_DEFAULTS *dfl = section_data;

  XZALLOC (op->v.hdrdel);
  return parse_regex_compat (&op->v.hdrdel->pat, dfl->re_type, dfl->re_type,
			     (dfl->ignore_case ? GENPAT_ICASE : 0));
}

static int
parse_set_header (void *call_data, void *section_data)
{
  return parse_rewrite_op (call_data, REWRITE_HDR_SET);
}

static int
parse_set_url (void *call_data, void *section_data)
{
  return parse_rewrite_op (call_data, REWRITE_URL_SET);
}

static int
parse_set_path (void *call_data, void *section_data)
{
  return parse_rewrite_op (call_data, REWRITE_PATH_SET);
}

static int
parse_set_query (void *call_data, void *section_data)
{
  return parse_rewrite_op (call_data, REWRITE_QUERY_SET);
}

static int
parse_set_query_param (void *call_data, void *section_data)
{
  REWRITE_OP *op = rewrite_op_alloc (call_data, REWRITE_QUERY_PARAM_SET);
  struct token *tok;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  op->v.qp.name = xstrdup (tok->str);

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  op->v.qp.value = xstrdup (tok->str);

  return CFGPARSER_OK;

}

static REWRITE_RULE *
rewrite_rule_alloc (REWRITE_RULE_HEAD *head)
{
  REWRITE_RULE *rule;

  XZALLOC (rule);
  service_cond_init (&rule->cond, COND_BOOL);
  SLIST_INIT (&rule->ophead);

  if (head)
    SLIST_PUSH (head, rule, next);

  return rule;
}

static int
parse_else (void *call_data, void *section_data)
{
  REWRITE_RULE *rule = rewrite_rule_alloc (NULL);
  *(REWRITE_RULE**)call_data = rule;
  return parser_loop (else_rule_parsetab, rule, section_data, NULL);
}

static int
parse_sub_rewrite (void *call_data, void *section_data)
{
  REWRITE_OP *op = rewrite_op_alloc (call_data, REWRITE_REWRITE_RULE);
  op->v.rule = rewrite_rule_alloc (NULL);
  return parser_loop (rewrite_rule_parsetab, op->v.rule, section_data, NULL);
}

static CFGPARSER_TABLE match_response_conditions[] = {
  {
    .name = "Header",
    .parser = parse_cond_hdr_matcher
  },
  {
    .name = "StringMatch",
    .parser = parse_cond_string_matcher
  },
  {
    .name = "Match",
    .parser = parse_match
  },
  {
    .name = "NOT",
    .parser = parse_not_cond
  },
  { NULL }
};

static CFGPARSER_TABLE rewrite_response_ops[] = {
  {
    .name = "SetHeader",
    .parser = parse_set_header
  },
  {
    .name = "DeleteHeader",
    .parser = parse_delete_header
  },
  { NULL },
};

static int parse_response_else (void *call_data, void *section_data);
static int parse_response_sub_rewrite (void *call_data, void *section_data);

static CFGPARSER_TABLE response_rewrite_rule_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "Rewrite",
    .parser = parse_response_sub_rewrite,
    .off = offsetof (REWRITE_RULE, ophead)
  },
  {
    .name = "Else",
    .parser = parse_response_else,
    .off = offsetof (REWRITE_RULE, iffalse)
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, cond),
    .type = KWT_SOFTREF,
    .ref = match_response_conditions
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, ophead),
    .type = KWT_SOFTREF,
    .ref = rewrite_response_ops
  },
  { NULL }
};

static CFGPARSER_TABLE response_else_rule_parsetab[] = {
  {
    .name = "End",
    .parser = parse_end_else
  },
  {
    .name = "Rewrite",
    .parser = parse_response_sub_rewrite,
    .off = offsetof (REWRITE_RULE, ophead)
  },
  {
    .name = "Else",
    .parser = parse_else,
    .off = offsetof (REWRITE_RULE, iffalse)
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, cond),
    .type = KWT_SOFTREF,
    .ref = match_response_conditions
  },
  {
    .name = "",
    .off = offsetof (REWRITE_RULE, ophead),
    .type = KWT_SOFTREF,
    .ref = rewrite_response_ops
  },
  { NULL }
};

static int
parse_response_else (void *call_data, void *section_data)
{
  REWRITE_RULE *rule = rewrite_rule_alloc (NULL);
  *(REWRITE_RULE**)call_data = rule;
  return parser_loop (response_else_rule_parsetab, rule, section_data, NULL);
}

static int
parse_response_sub_rewrite (void *call_data, void *section_data)
{
  REWRITE_OP *op = rewrite_op_alloc (call_data, REWRITE_REWRITE_RULE);
  op->v.rule = rewrite_rule_alloc (NULL);
  return parser_loop (response_rewrite_rule_parsetab, op->v.rule, section_data, NULL);
}

static int
parse_rewrite (void *call_data, void *section_data)
{
  struct token *tok;
  CFGPARSER_TABLE *table;
  REWRITE_RULE_HEAD *rw = call_data, *head;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;
  if (tok->type == T_IDENT)
    {
      if (strcasecmp (tok->str, "response") == 0)
	{
	  table = response_rewrite_rule_parsetab;
	  head = &rw[REWRITE_RESPONSE];
	}
      else if (strcasecmp (tok->str, "request") == 0)
	{
	  table = rewrite_rule_parsetab;
	  head = &rw[REWRITE_REQUEST];
	}
      else
	{
	  conf_error ("expected response, request, or newline, but found %s",
		      token_type_str (tok->type));
	  return CFGPARSER_FAIL;
	}
    }
  else
    {
      putback_tkn (tok);
      table = rewrite_rule_parsetab;
      head = &rw[REWRITE_REQUEST];
    }
  return parser_loop (table, rewrite_rule_alloc (head), section_data, NULL);
}

static REWRITE_RULE *
rewrite_rule_last_uncond (REWRITE_RULE_HEAD *head)
{
  if (!SLIST_EMPTY (head))
    {
      REWRITE_RULE *rw = SLIST_LAST (head);
      if (rw->cond.type == COND_BOOL && SLIST_EMPTY (&rw->cond.bool.head))
	return rw;
    }

  return rewrite_rule_alloc (head);
}

#define __cat2__(a,b) a ## b
#define SETFN_NAME(part)			\
  __cat2__(parse_,part)
#define SETFN_SVC_NAME(part)			\
  __cat2__(parse_svc_,part)
#define SETFN_SVC_DECL(part)					     \
  static int							     \
  SETFN_SVC_NAME(part) (void *call_data, void *section_data)	     \
  {								     \
    REWRITE_RULE *rule = rewrite_rule_last_uncond (call_data);	     \
    return SETFN_NAME(part) (&rule->ophead, section_data);	     \
  }

SETFN_SVC_DECL (set_url)
SETFN_SVC_DECL (set_path)
SETFN_SVC_DECL (set_query)
SETFN_SVC_DECL (set_query_param)
SETFN_SVC_DECL (set_header)
SETFN_SVC_DECL (delete_header)

/*
 * Support for backward-compatible HeaderRemove and HeadRemove directives.
 */
static int
parse_header_remove (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  REWRITE_RULE *rule = rewrite_rule_last_uncond (call_data);
  REWRITE_OP *op = rewrite_op_alloc (&rule->ophead, REWRITE_HDR_DEL);
  XZALLOC (op->v.hdrdel);
  return parse_regex_compat (&op->v.hdrdel->pat, dfl->re_type, dfl->re_type,
			     GENPAT_ICASE | GENPAT_MULTILINE);
}

static int
parse_balancer (void *call_data, void *section_data)
{
  BALANCER_ALGO *t = call_data;
  struct token *tok;

  if ((tok = gettkn_expect_mask (T_UNQ)) == NULL)
    return CFGPARSER_FAIL;
  if (strcasecmp (tok->str, "random") == 0)
    *t = BALANCER_ALGO_RANDOM;
  else if (strcasecmp (tok->str, "iwrr") == 0)
    *t = BALANCER_ALGO_IWRR;
  else
    {
      conf_error ("unsupported balancing strategy: %s", tok->str);
      return CFGPARSER_FAIL;
    }
  return CFGPARSER_OK;
}

static int
parse_log_suppress (void *call_data, void *section_data)
{
  int *result_ptr = call_data;
  struct token *tok;
  int n;
  int result = 0;
  static struct kwtab status_table[] = {
    { "all",      STATUS_MASK (100) | STATUS_MASK (200) |
		  STATUS_MASK (300) | STATUS_MASK (400) | STATUS_MASK (500) },
    { "info",     STATUS_MASK (100) },
    { "success",  STATUS_MASK (200) },
    { "redirect", STATUS_MASK (300) },
    { "clterr",   STATUS_MASK (400) },
    { "srverr",   STATUS_MASK (500) },
    { NULL }
  };

  if ((tok = gettkn_expect_mask (T_UNQ)) == NULL)
    return CFGPARSER_FAIL;

  do
    {
      if (strlen (tok->str) == 1 && isdigit (tok->str[0]))
	{
	  n = tok->str[0] - '0';
	  if (n <= 0 || n >= sizeof (status_table) / sizeof (status_table[0]))
	    {
	      conf_error ("%s", "unsupported status mask");
	      return CFGPARSER_FAIL;
	    }
	  n = STATUS_MASK (n * 100);
	}
      else if (kw_to_tok (status_table, tok->str, 1, &n) != 0)
	{
	  conf_error ("%s", "unsupported status mask");
	  return CFGPARSER_FAIL;
	}
      result |= n;
    }
  while ((tok = gettkn_any ()) != NULL && tok->type != T_ERROR &&
	 T_MASK_ISSET (T_UNQ, tok->type));

  if (tok == NULL)
    {
      conf_error ("%s", "unexpected end of file");
      return CFGPARSER_FAIL;
    }
  if (tok->type == T_ERROR)
    return CFGPARSER_FAIL;

  putback_tkn (tok);

  *result_ptr = result;

  return CFGPARSER_OK;
}

static CFGPARSER_TABLE service_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },

  {
    .name = "",
    .off = offsetof (SERVICE, cond),
    .type = KWT_SOFTREF,
    .ref = match_conditions
  },
  {
    .name = "Rewrite",
    .parser = parse_rewrite,
    .off = offsetof (SERVICE, rewrite)
  },
  {
    .name = "SetHeader",
    .parser = SETFN_SVC_NAME (set_header),
    .off = offsetof (SERVICE, rewrite)
  },
  {
    .name = "DeleteHeader",
    .parser = SETFN_SVC_NAME (delete_header),
    .off = offsetof (SERVICE, rewrite)
  },
  {
    .name = "SetURL",
    .parser = SETFN_SVC_NAME (set_url),
    .off = offsetof (SERVICE, rewrite)
  },
  {
    .name = "SetPath",
    .parser = SETFN_SVC_NAME (set_path),
    .off = offsetof (SERVICE, rewrite)
  },
  {
    .name = "SetQuery",
    .parser = SETFN_SVC_NAME (set_query),
    .off = offsetof (SERVICE, rewrite)
  },
  {
    .name = "SetQueryParam",
    .parser = SETFN_SVC_NAME (set_query_param),
    .off = offsetof (SERVICE, rewrite)
  },

  {
    .name = "Disabled",
    .parser = cfg_assign_bool,
    .off = offsetof (SERVICE, disabled)
  },
  {
    .name = "Redirect",
    .parser = parse_redirect_backend,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "Error",
    .parser = parse_error_backend,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "Backend",
    .parser = parse_backend,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "UseBackend",
    .parser = parse_use_backend,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "Emergency",
    .parser = parse_emergency,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "Metrics",
    .parser = parse_metrics,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "Control",
    .parser = parse_control_backend,
    .off = offsetof (SERVICE, backends)
  },
  {
    .name = "Session",
    .parser = parse_session
  },
  {
    .name = "Balancer",
    .parser = parse_balancer,
    .off = offsetof (SERVICE, balancer_algo)
  },
  {
    .name = "ForwardedHeader",
    .parser = cfg_assign_string,
    .off = offsetof (SERVICE, forwarded_header)
  },
  {
    .name = "TrustedIP",
    .parser = assign_acl,
    .off = offsetof (SERVICE, trusted_ips)
  },
  {
    .name = "LogSuppress",
    .parser = parse_log_suppress,
    .off = offsetof (SERVICE, log_suppress_mask)
  },

  /* Backward compatibility */
  {
    .name = "IgnoreCase",
    .parser = assign_dfl_ignore_case,
    .deprecated = 1,
    .message = "use the -icase matching directive flag to request case-insensitive comparison"
  },

  { NULL }
};

static int
find_service_ident (SERVICE_HEAD *svc_head, char const *name)
{
  SERVICE *svc;
  SLIST_FOREACH (svc, svc_head, next)
    {
      if (svc->name && strcmp (svc->name, name) == 0)
	return 1;
    }
  return 0;
}

static SERVICE *
new_service (BALANCER_ALGO algo)
{
  SERVICE *svc;

  XZALLOC (svc);

  service_cond_init (&svc->cond, COND_BOOL);
  DLIST_INIT (&svc->backends);

  svc->sess_type = SESS_NONE;
  pthread_mutex_init (&svc->mut, &mutex_attr_recursive);
  svc->balancer_algo = algo;

  DLIST_INIT (&svc->be_rem_head);
  pthread_cond_init (&svc->be_rem_cond, NULL);
  
  return svc;
}

static int backend_pri_max[] = {
  [BALANCER_ALGO_RANDOM] = PRI_MAX_RANDOM,
  [BALANCER_ALGO_IWRR]   = PRI_MAX_IWRR
};
   
static int
parse_service (void *call_data, void *section_data)
{
  SERVICE_HEAD *head = call_data;
  POUND_DEFAULTS *dfl = (POUND_DEFAULTS*) section_data;
  struct token *tok;
  SERVICE *svc;
  struct locus_range range;

  svc = new_service (dfl->balancer_algo);
  
  tok = gettkn_any ();

  if (!tok)
    return CFGPARSER_FAIL;

  if (tok->type == T_STRING)
    {
      if (find_service_ident (head, tok->str))
	{
	  conf_error ("%s", "service name is not unique");
	  return CFGPARSER_FAIL;
	}
      svc->name = xstrdup (tok->str);
    }
  else
    putback_tkn (tok);

  if ((svc->sessions = session_table_new ()) == NULL)
    {
      conf_error ("%s", "session_table_new failed");
      return CFGPARSER_FAIL;
    }

  if (parser_loop (service_parsetab, svc, dfl, &range))
    return CFGPARSER_FAIL;
  else
    {
      BALANCER *be_list;
      unsigned be_count = 0;
      
      DLIST_FOREACH (be_list, &svc->backends, link)
	{
	  BACKEND *be;	  
	  int be_class = 0;
#         define BE_MASK(n) (1<<(n))
#         define  BX_(x)  ((x) - (((x)>>1)&0x77777777)			\
			   - (((x)>>2)&0x33333333)			\
			   - (((x)>>3)&0x11111111))
#         define BITCOUNT(x)     (((BX_(x)+(BX_(x)>>4)) & 0x0F0F0F0F) % 255)
	  int n = 0;
	  int pri_max = backend_pri_max[svc->balancer_algo];

	  be_list->tot_pri = 0;
	  be_list->max_pri = 0;
	  DLIST_FOREACH (be, &be_list->backends, link)
	    {
	      n++;
	      if (be->priority > pri_max)
		{
		  conf_error_at_locus_range (&be->locus,
					     "backend priority out of allowed"
					     " range; reset to max. %d",
					     pri_max);
		  be->priority = pri_max;
		}
	      be_class |= BE_MASK (be->be_type);
	      be->service = svc;
	      if (!be->disabled)
		{
		  if (TOT_PRI_MAX - be_list->tot_pri > be->priority)
		    be_list->tot_pri += be->priority;
		  else
		    {
		      conf_error_at_locus_range (&be->locus,
						 "this backend overflows the"
						 " sum of priorities");
		      return CFGPARSER_FAIL;
		    }
		  if (be_list->max_pri < be->priority)
		    be_list->max_pri = be->priority;
		}
	    }

	  if (n > 1)
	    {
	      if (be_class & ~(BE_MASK (BE_REGULAR) |
			       BE_MASK (BE_MATRIX) |
			       BE_MASK (BE_REDIRECT)))
		{
		  conf_error_at_locus_range (&range,
			  "%s",
			  BITCOUNT (be_class) == 1
			    ? "multiple backends of this type are not allowed"
			    : "service mixes backends of different types");
		  return CFGPARSER_FAIL;
		}

	       if (be_class & BE_MASK (BE_REDIRECT))
		{
		  conf_error_at_locus_range (&range,
			  "warning: %s",
			  (be_class & (BE_MASK (BE_REGULAR) |
				       BE_MASK (BE_MATRIX)))
			     ? "service mixes regular and redirect backends"
			     : "service uses multiple redirect backends");
		  conf_error_at_locus_range (&range,
			  "see section \"DEPRECATED FEATURES\" in pound(8)");
		}
	    }
	  
	  be_count += n;
	}

      if (be_count == 0)
	{
	  conf_error_at_locus_range (&range, "warning: no backends defined");
	}
      
      service_lb_init (svc);

      SLIST_PUSH (head, svc, next);
    }
  svc->locus_str = format_locus_str (&range);
  return CFGPARSER_OK;
}

static int
parse_acme (void *call_data, void *section_data)
{
  SERVICE_HEAD *head = call_data;
  SERVICE *svc;
  BACKEND *be;
  SERVICE_COND *cond;
  struct token *tok;
  struct stat st;
  int rc;
  static char sp_acme[] = "^/\\.well-known/acme-challenge/(.+)";
  int fd;
  struct locus_range range;

  range.beg = last_token_locus_range ()->beg;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  if (stat (tok->str, &st))
    {
      conf_error ("can't stat %s: %s", tok->str, strerror (errno));
      return CFGPARSER_FAIL;
    }
  if (!S_ISDIR (st.st_mode))
    {
      conf_error ("%s is not a directory: %s", tok->str, strerror (errno));
      return CFGPARSER_FAIL;
    }
  if ((fd = open (tok->str, O_RDONLY | O_NONBLOCK | O_DIRECTORY)) == -1)
    {
      conf_error ("can't open directory %s: %s", tok->str, strerror (errno));
      return CFGPARSER_FAIL;
    }

  /* Create service; there'll be only one backend so the balancing algorithm
     doesn't really matter. */
  svc = new_service (BALANCER_ALGO_RANDOM);

  /* Create a URL matcher */
  cond = service_cond_append (&svc->cond, COND_URL);
  rc = genpat_compile (&cond->re, GENPAT_POSIX, sp_acme, 0);
  if (rc)
    {
      conf_regcomp_error (rc, cond->re, NULL);
      return CFGPARSER_FAIL;
    }

  range.end = last_token_locus_range ()->beg;
  svc->locus_str = format_locus_str (&range);

  /* Create ACME backend */
  XZALLOC (be);
  be->be_type = BE_ACME;
  be->priority = 1;
  pthread_mutex_init (&be->mut, NULL);

  be->v.acme.wd = fd;

  /* Register backend in service */
  balancer_add_backend (balancer_list_get_normal (&svc->backends), be);
  service_recompute_pri_unlocked (svc, NULL, NULL);

  /* Register service in the listener */
  SLIST_PUSH (head, svc, next);

  return CFGPARSER_OK;
}


static int
listener_parse_xhttp (void *call_data, void *section_data)
{
  return cfg_assign_int_range (call_data, 0, 3);
}

static int
listener_parse_checkurl (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  POUND_DEFAULTS *dfl = section_data;

  if (lst->url_pat)
    {
      conf_error ("%s", "CheckURL multiple pattern");
      return CFGPARSER_FAIL;
    }

  return parse_regex_compat (&lst->url_pat, dfl->re_type, dfl->re_type,
			     (dfl->ignore_case ? GENPAT_ICASE : 0));
}

static int
read_fd (int fd)
{
  struct msghdr msg;
  struct iovec iov[1];
  char base[1];
  union
  {
    struct cmsghdr cm;
    char control[CMSG_SPACE (sizeof (int))];
  } control_un;
  struct cmsghdr *cmptr;

  msg.msg_control = control_un.control;
  msg.msg_controllen = sizeof (control_un.control);

  msg.msg_name = NULL;
  msg.msg_namelen = 0;

  iov[0].iov_base = base;
  iov[0].iov_len = sizeof (base);

  msg.msg_iov = iov;
  msg.msg_iovlen = 1;
  if (recvmsg (fd, &msg, 0) > 0)
    {
      if ((cmptr = CMSG_FIRSTHDR (&msg)) != NULL
	  && cmptr->cmsg_len == CMSG_LEN (sizeof (int))
	  && cmptr->cmsg_level == SOL_SOCKET
	  && cmptr->cmsg_type == SCM_RIGHTS)
	return *((int*) CMSG_DATA (cmptr));
    }
  return -1;
}

static int
listener_parse_socket_from (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct sockaddr_storage ss;
  socklen_t sslen = sizeof (ss);
  struct token *tok;
  struct addrinfo addr;
  int sfd, fd;

  if (ADDRINFO_HAS_ADDRESS (&lst->addr))
    {
      conf_error ("%s", "Duplicate Address or SocketFrom statement");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  memset (&addr, 0, sizeof (addr));
  if (assign_address_internal (&addr, tok) != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  if ((sfd = socket (PF_UNIX, SOCK_STREAM, 0)) < 0)
    {
      conf_error ("socket: %s", strerror (errno));
      return CFGPARSER_FAIL;
    }

  if (connect (sfd, addr.ai_addr, addr.ai_addrlen) < 0)
    {
      conf_error ("connect %s: %s",
		  ((struct sockaddr_un*)addr.ai_addr)->sun_path,
		  strerror (errno));
      return CFGPARSER_FAIL;
    }

  fd = read_fd (sfd);

  if (fd == -1)
    {
      conf_error ("can't get socket: %s", strerror (errno));
      return CFGPARSER_FAIL;
    }

  if (getsockname (fd, (struct sockaddr*) &ss, &sslen) == -1)
    {
      conf_error ("can't get socket address: %s", strerror (errno));
      return CFGPARSER_FAIL;
    }

  free (lst->addr.ai_addr);
  lst->addr.ai_addr = xmalloc (sslen);
  memcpy (lst->addr.ai_addr, &ss, sslen);
  lst->addr.ai_addrlen = sslen;
  lst->addr.ai_family = ss.ss_family;
  ADDRINFO_SET_ADDRESS (&lst->addr);
  ADDRINFO_SET_PORT (&lst->addr);

  {
    struct stringbuf sb;
    char tmp[MAX_ADDR_BUFSIZE];

    xstringbuf_init (&sb);
    stringbuf_format_locus_range (&sb, &tok->locus);
    stringbuf_add_string (&sb, ": obtained address ");
    stringbuf_add_string (&sb, addr2str (tmp, sizeof (tmp), &lst->addr, 0));
    logmsg (LOG_DEBUG, "%s", stringbuf_finish (&sb));
    stringbuf_free (&sb);
  }

  lst->sock = fd;

  return CFGPARSER_OK;
}

static int
parse_rewritelocation (void *call_data, void *section_data)
{
  return cfg_assign_int_range (call_data, 0, 2);
}

struct canned_log_format
{
  char *name;
  char *fmt;
};

static struct canned_log_format canned_log_format[] = {
  /* 0 - not used */
  { "null", "" },
  /* 1 - regular logging */
  { "regular", "%a %r - %>s" },
  /* 2 - extended logging (show chosen backend server as well) */
  { "extended", "%a %r - %>s (%{Host}i/%{service}N -> %{backend}N) %{f}T sec" },
  /* 3 - Apache-like format (Combined Log Format with Virtual Host) */
  { "vhost_combined", "%{Host}I %a - %u %t \"%r\" %s %b \"%{Referer}i\" \"%{User-Agent}i\"" },
  /* 4 - same as 3 but without the virtual host information */
  { "combined", "%a - %u %t \"%r\" %s %b \"%{Referer}i\" \"%{User-Agent}i\"" },
  /* 5 - same as 3 but with information about the Service and Backend used */
  { "detailed", "%{Host}I %a - %u %t \"%r\" %s %b \"%{Referer}i\" \"%{User-Agent}i\" (%{service}N -> %{backend}N) %{f}T sec" },
};
static int max_canned_log_format =
  sizeof (canned_log_format) / sizeof (canned_log_format[0]);

struct log_format_data
{
  struct locus_range *locus;
  int fn;
  int fatal;
};

void
log_format_diag (void *data, int fatal, char const *msg, int off)
{
  struct log_format_data *ld = data;
  if (ld->fn == -1)
    {
      struct locus_range loc = *ld->locus;
      loc.beg.col += off;
      loc.end = loc.beg;
      conf_error_at_locus_range (&loc, "%s", msg);
    }
  else
    {
      conf_error_at_locus_range (ld->locus, "INTERNAL ERROR: error compiling built-in format %d", ld->fn);
      conf_error_at_locus_range (ld->locus, "%s: near %s", msg,
				 canned_log_format[ld->fn].fmt + off);
      conf_error_at_locus_range (ld->locus, "please report");
    }
  ld->fatal = fatal;
}

static void
compile_canned_formats (void)
{
  struct log_format_data ld;
  int i;

  ld.locus = NULL;
  ld.fatal = 0;

  for (i = 0; i < max_canned_log_format; i++)
    {
      ld.fn = i;
      if (http_log_format_compile (canned_log_format[i].name,
				   canned_log_format[i].fmt,
				   log_format_diag, &ld) == -1 || ld.fatal)
	exit (1);
    }
}

static int
parse_log_level (void *call_data, void *section_data)
{
  int log_level;
  int *log_level_ptr = call_data;
  struct token *tok = gettkn_expect_mask (T_BIT (T_STRING) | T_BIT (T_NUMBER));
  if (!tok)
    return CFGPARSER_FAIL;

  if (tok->type == T_STRING)
    {
      log_level = http_log_format_find (tok->str);
      if (log_level == -1)
	{
	  conf_error ("undefined format: %s", tok->str);
	  return CFGPARSER_FAIL;
	}
    }
  else
    {
      char *p;
      long n;

      errno = 0;
      n = strtol (tok->str, &p, 10);
      if (errno || *p || n < 0 || n > INT_MAX)
	{
	  conf_error ("%s", "unsupported log level number");
	  return CFGPARSER_FAIL;
	}
      if (http_log_format_check (n))
	{
	  conf_error ("%s", "undefined log level");
	  return CFGPARSER_FAIL;
	}
      log_level = n;
    }
  *log_level_ptr = log_level;
  return CFGPARSER_OK;
}

static int
parse_log_format (void *call_data, void *section_data)
{
  struct token *tok;
  char *name;
  struct log_format_data ld;
  int rc;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;
  name = strdup (tok->str);
  if ((tok = gettkn_expect (T_STRING)) == NULL)
    {
      free (name);
      return CFGPARSER_FAIL;
    }

  ld.locus = &tok->locus;
  ld.fn = -1;
  ld.fatal = 0;

  if (http_log_format_compile (name, tok->str, log_format_diag, &ld) == -1 ||
      ld.fatal)
    rc = CFGPARSER_FAIL;
  else
    rc = CFGPARSER_OK;
  free (name);
  return rc;
}

static int
parse_header_options (void *call_data, void *section_data)
{
  int *opt = call_data;
  int n;
  struct token *tok;
  static struct kwtab options[] = {
    { "forwarded", HDROPT_FORWARDED_HEADERS },
    { "ssl",       HDROPT_SSL_HEADERS },
    { "all",       HDROPT_FORWARDED_HEADERS|HDROPT_SSL_HEADERS },
    { NULL }
  };

  for (;;)
    {
      char *name;
      int neg;

      if ((tok = gettkn_any ()) == NULL)
	return CFGPARSER_FAIL;
      if (tok->type == '\n')
	break;
      if (!(tok->type == T_IDENT || tok->type == T_LITERAL))
	{
	  conf_error ("unexpected %s", token_type_str (tok->type));
	  return CFGPARSER_FAIL;
	}

      name = tok->str;
      if (strcasecmp (name, "none") == 0)
	*opt = 0;
      else
	{
	  if (strncasecmp (name, "no-", 3) == 0)
	    {
	      neg = 1;
	      name += 3;
	    }
	  else
	    neg = 0;

	  if (kw_to_tok (options, name, 1, &n))
	    {
	      conf_error ("%s", "unknown option");
	      return CFGPARSER_FAIL;
	    }

	  if (neg)
	    *opt &= ~n;
	  else
	    *opt |= n;
	}
    }

  return CFGPARSER_OK_NONL;
}

static CFGPARSER_TABLE http_common[] = {
  {
    .name = "Address",
    .parser = assign_address,
    .off = offsetof (LISTENER, addr)
  },
  {
    .name = "Port",
    .parser = assign_port_addrinfo,
    .off = offsetof (LISTENER, addr)
  },
  {
    .name = "SocketFrom",
    .parser = listener_parse_socket_from
  },
  {
    .name = "xHTTP",
    .parser = listener_parse_xhttp,
    .off = offsetof (LISTENER, verb)
  },
  {
    .name = "Client",
    .parser = cfg_assign_timeout,
    .off = offsetof (LISTENER, to)
  },
  {
    .name = "CheckURL",
    .parser = listener_parse_checkurl
  },
  {
    .name = "ErrorFile",
    .parser = parse_errorfile,
    .off = offsetof (LISTENER, http_err)
  },
  {
    .name = "MaxRequest",
    .parser = assign_CONTENT_LENGTH,
    .off = offsetof (LISTENER, max_req_size)
  },
  {
    .name = "MaxURI",
    .parser = cfg_assign_unsigned,
    .off = offsetof (LISTENER, max_uri_length)
  },

  {
    .name = "Rewrite",
    .parser = parse_rewrite,
    .off = offsetof (LISTENER, rewrite)
  },
  {
    .name = "SetHeader",
    .parser = SETFN_SVC_NAME (set_header),
    .off = offsetof (LISTENER, rewrite)
  },
  {
    .name = "HeaderAdd",
    .type = KWT_ALIAS,
    .deprecated = 1
  },
  {
    .name = "AddHeader",
    .type = KWT_ALIAS,
    .deprecated = 1
  },
  {
    .name = "DeleteHeader",
    .parser = SETFN_SVC_NAME (delete_header),
    .off = offsetof (LISTENER, rewrite)
  },
  {
    .name = "HeaderRemove",
    .parser = parse_header_remove,
    .off = offsetof (LISTENER, rewrite),
    .deprecated = 1,
    .message = "use \"DeleteHeader\" instead"
  },
  {
    .name = "HeadRemove",
    .type = KWT_ALIAS,
    .deprecated = 1,
    .message = "use \"DeleteHeader\" instead"
  },
  {
    .name = "SetURL",
    .parser = SETFN_SVC_NAME (set_url),
    .off = offsetof (LISTENER, rewrite)
  },
  {
    .name = "SetPath",
    .parser = SETFN_SVC_NAME (set_path),
    .off = offsetof (LISTENER, rewrite)
  },
  {
    .name = "SetQuery",
    .parser = SETFN_SVC_NAME (set_query),
    .off = offsetof (LISTENER, rewrite)
  },
  {
    .name = "SetQueryParam",
    .parser = SETFN_SVC_NAME (set_query_param),
    .off = offsetof (LISTENER, rewrite)
  },

  {
    .name = "HeaderOption",
    .parser = parse_header_options,
    .off = offsetof (LISTENER, header_options)
  },

  {
    .name = "RewriteLocation",
    .parser = parse_rewritelocation,
    .off = offsetof (LISTENER, rewr_loc)
  },
  {
    .name = "RewriteDestination",
    .parser = cfg_assign_bool,
    .off = offsetof (LISTENER, rewr_dest)
  },
  {
    .name = "LogLevel",
    .parser = parse_log_level,
    .off = offsetof (LISTENER, log_level)
  },
  {
    .name = "ForwardedHeader",
    .parser = cfg_assign_string,
    .off = offsetof (LISTENER, forwarded_header)
  },
  {
    .name = "TrustedIP",
    .parser = assign_acl,
    .off = offsetof (LISTENER, trusted_ips)
  },
  {
    .name = "Service",
    .parser = parse_service,
    .off = offsetof (LISTENER, services)
  },
  { NULL }
};

static CFGPARSER_TABLE http_deprecated[] = {
  /* Backward compatibility */
  {
    .name = "Err400",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_BAD_REQUEST]),
    .deprecated = 1,
    .message = "use \"ErrorFile 400\" instead"
  },
  {
    .name = "Err401",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_UNAUTHORIZED]),
    .deprecated = 1,
    .message = "use \"ErrorFile 401\" instead"
  },
  {
    .name = "Err403",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_FORBIDDEN]),
    .deprecated = 1,
    .message = "use \"ErrorFile 403\" instead"
  },
  {
    .name = "Err404",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_NOT_FOUND]),
    .deprecated = 1,
    .message = "use \"ErrorFile 404\" instead"
  },
  {
    .name = "Err413",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_PAYLOAD_TOO_LARGE]),
    .deprecated = 1,
    .message = "use \"ErrorFile 413\" instead"
  },
  {
    .name = "Err414",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_URI_TOO_LONG]),
    .deprecated = 1,
    .message = "use \"ErrorFile 414\" instead"
  },
  {
    .name = "Err500",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_INTERNAL_SERVER_ERROR]),
    .deprecated = 1,
    .message = "use \"ErrorFile 500\" instead"
  },
  {
    .name = "Err501",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_NOT_IMPLEMENTED]),
    .deprecated = 1,
    .message = "use \"ErrorFile 501\" instead"
  },
  {
    .name = "Err503",
    .parser = cfg_assign_string_from_file,
    .off = offsetof (LISTENER, http_err[HTTP_STATUS_SERVICE_UNAVAILABLE]),
    .deprecated = 1,
    .message = "use \"ErrorFile 503\" instead"
  },

  { NULL }
};

static CFGPARSER_TABLE http_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "",
    .type = KWT_TABREF,
    .ref = http_common
  },
  {
    .name = "",
    .type = KWT_TABREF,
    .ref = http_deprecated
  },
  {
    .name = "ACME",
    .parser = parse_acme,
    .off = offsetof (LISTENER, services)
  },

  { NULL }
};

static LISTENER *
listener_alloc (POUND_DEFAULTS *dfl)
{
  LISTENER *lst;

  XZALLOC (lst);

  lst->mode = 0600;
  lst->sock = -1;
  lst->to = dfl->clnt_to;
  lst->rewr_loc = 1;
  lst->log_level = dfl->log_level;
  lst->verb = 0;
  lst->header_options = dfl->header_options;
  SLIST_INIT (&lst->rewrite[REWRITE_REQUEST]);
  SLIST_INIT (&lst->rewrite[REWRITE_RESPONSE]);
  SLIST_INIT (&lst->services);
  SLIST_INIT (&lst->ctx_head);
  return lst;
}

static int
find_listener_ident (LISTENER_HEAD *list_head, char const *name)
{
  LISTENER *lstn;
  SLIST_FOREACH (lstn, list_head, next)
    {
      if (lstn->name && strcmp (lstn->name, name) == 0)
	return 1;
    }
  return 0;
}

static int
parse_listen_http (void *call_data, void *section_data)
{
  LISTENER *lst;
  LISTENER_HEAD *list_head = call_data;
  POUND_DEFAULTS *dfl = section_data;
  struct locus_range range;
  struct token *tok;

  if ((lst = listener_alloc (dfl)) == NULL)
    return CFGPARSER_FAIL;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;
  else if (tok->type == T_STRING)
    {
      if (find_listener_ident (list_head, tok->str))
	{
	  conf_error ("%s", "listener name is not unique");
	  return CFGPARSER_FAIL;
	}
      lst->name = xstrdup (tok->str);
    }
  else
    putback_tkn (tok);

  if (parser_loop (http_parsetab, lst, section_data, &range))
    return CFGPARSER_FAIL;

  if (check_addrinfo (&lst->addr, &range, "ListenHTTP") != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  lst->locus_str = format_locus_str (&range);

  SLIST_PUSH (list_head, lst, next);
  return CFGPARSER_OK;
}

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
# define general_name_string(n) \
	xstrndup ((char*)ASN1_STRING_get0_data (n->d.dNSName),	\
		 ASN1_STRING_length (n->d.dNSName) + 1)
#else
# define general_name_string(n) \
	xstrndup ((char*)ASN1_STRING_data(n->d.dNSName),	\
		 ASN1_STRING_length (n->d.dNSName) + 1)
#endif

static void
get_subjectaltnames (X509 *x509, POUND_CTX *pc, size_t san_max)
{
  STACK_OF (GENERAL_NAME) * san_stack =
    (STACK_OF (GENERAL_NAME) *) X509_get_ext_d2i (x509, NID_subject_alt_name,
						  NULL, NULL);
  char **result;

  if (san_stack == NULL)
    return;
  while (sk_GENERAL_NAME_num (san_stack) > 0)
    {
      GENERAL_NAME *name = sk_GENERAL_NAME_pop (san_stack);
      switch (name->type)
	{
	case GEN_DNS:
	  if (pc->subjectAltNameCount == san_max)
	    pc->subjectAltNames = x2nrealloc (pc->subjectAltNames,
					      &san_max,
					      sizeof (pc->subjectAltNames[0]));
	  pc->subjectAltNames[pc->subjectAltNameCount++] = general_name_string (name);
	  break;

	default:
	  logmsg (LOG_INFO, "unsupported subjectAltName type encountered: %i",
		  name->type);
	}
      GENERAL_NAME_free (name);
    }

  sk_GENERAL_NAME_pop_free (san_stack, GENERAL_NAME_free);
  if (pc->subjectAltNameCount
      && (result = realloc (pc->subjectAltNames,
			    pc->subjectAltNameCount * sizeof (pc->subjectAltNames[0]))) != NULL)
    pc->subjectAltNames = result;
}

static int
load_cert (char const *filename, LISTENER *lst)
{
  POUND_CTX *pc;

  XZALLOC (pc);

  if ((pc->ctx = SSL_CTX_new (SSLv23_server_method ())) == NULL)
    {
      conf_openssl_error (NULL, "SSL_CTX_new");
      return CFGPARSER_FAIL;
    }

  if (SSL_CTX_use_certificate_chain_file (pc->ctx, filename) != 1)
    {
      conf_openssl_error (filename, "SSL_CTX_use_certificate_chain_file");
      return CFGPARSER_FAIL;
    }
  if (SSL_CTX_use_PrivateKey_file (pc->ctx, filename, SSL_FILETYPE_PEM) != 1)
    {
      conf_openssl_error (filename, "SSL_CTX_use_PrivateKey_file");
      return CFGPARSER_FAIL;
    }

  if (SSL_CTX_check_private_key (pc->ctx) != 1)
    {
      conf_openssl_error (filename, "SSL_CTX_check_private_key");
      return CFGPARSER_FAIL;
    }

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  {
    /* we have support for SNI */
    FILE *fcert;
    X509 *x509;
    X509_NAME *xname = NULL;
    int i;
    size_t san_max;

    if ((fcert = fopen (filename, "r")) == NULL)
      {
	conf_error ("%s: could not open certificate file: %s", filename,
		    strerror (errno));
	return CFGPARSER_FAIL;
      }

    x509 = PEM_read_X509 (fcert, NULL, NULL, NULL);
    fclose (fcert);

    if (!x509)
      {
	conf_error ("%s: could not get certificate subject", filename);
	return CFGPARSER_FAIL;
      }

    pc->subjectAltNameCount = 0;
    pc->subjectAltNames = NULL;
    san_max = 0;

    /* Extract server name */
    xname = X509_get_subject_name (x509);
    for (i = -1;
	 (i = X509_NAME_get_index_by_NID (xname, NID_commonName, i)) != -1;)
      {
	X509_NAME_ENTRY *entry = X509_NAME_get_entry (xname, i);
	ASN1_STRING *value;
	char *str = NULL;
	value = X509_NAME_ENTRY_get_data (entry);
	if (ASN1_STRING_to_UTF8 ((unsigned char **)&str, value) >= 0)
	  {
	    if (pc->server_name == NULL)
	      pc->server_name = str;
	    else
	      {
		if (pc->subjectAltNameCount == san_max)
		  pc->subjectAltNames = x2nrealloc (pc->subjectAltNames,
						    &san_max,
						    sizeof (pc->subjectAltNames[0]));
		pc->subjectAltNames[pc->subjectAltNameCount++] = str;
	      }
	  }
      }

    get_subjectaltnames (x509, pc, san_max);
    X509_free (x509);

    if (pc->server_name == NULL)
      {
	conf_error ("%s: no CN in certificate subject name", filename);
	return CFGPARSER_FAIL;
      }
  }
#else
  if (res->ctx)
    conf_error ("%s: multiple certificates not supported", filename);
#endif
  SLIST_PUSH (&lst->ctx_head, pc, next);

  return CFGPARSER_OK;
}

static int
https_parse_cert (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  struct stat st;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  if (stat (tok->str, &st))
    {
      conf_error ("%s: stat error: %s", tok->str, strerror (errno));
      return CFGPARSER_FAIL;
    }

  if (S_ISREG (st.st_mode))
    return load_cert (tok->str, lst);

  if (S_ISDIR (st.st_mode))
    {
      DIR *dp;
      struct dirent *ent;
      struct stringbuf namebuf;
      size_t dirlen;
      int rc = CFGPARSER_OK;

      dirlen = strlen (tok->str);
      while (dirlen > 0 && tok->str[dirlen-1] == '/')
	dirlen--;

      xstringbuf_init (&namebuf);
      stringbuf_add (&namebuf, tok->str, dirlen);
      stringbuf_add_char (&namebuf, '/');
      dirlen++;

      dp = opendir (tok->str);
      if (dp == NULL)
	{
	  conf_error ("%s: error opening directory: %s", tok->str,
		      strerror (errno));
	  stringbuf_free (&namebuf);
	  return CFGPARSER_FAIL;
	}

      while ((ent = readdir (dp)) != NULL)
	{
	  char *filename;

	  if (strcmp (ent->d_name, ".") == 0 || strcmp (ent->d_name, "..") == 0)
	    continue;

	  stringbuf_add_string (&namebuf, ent->d_name);
	  filename = stringbuf_finish (&namebuf);
	  if (stat (filename, &st))
	    {
	      conf_error ("%s: stat error: %s", filename, strerror (errno));
	    }
	  else if (S_ISREG (st.st_mode))
	    {
	      if ((rc = load_cert (filename, lst)) != CFGPARSER_OK)
		break;
	    }
	  else
	    conf_error ("warning: ignoring %s: not a regular file", filename);
	  stringbuf_truncate (&namebuf, dirlen);
	}
      closedir (dp);
      stringbuf_free (&namebuf);
      return rc;
    }

  conf_error ("%s: not a regular file or directory", tok->str);
  return CFGPARSER_FAIL;
}

static int
verify_OK (int pre_ok, X509_STORE_CTX * ctx)
{
  return 1;
}

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
static int
SNI_server_name (SSL *ssl, int *dummy, POUND_CTX_HEAD *ctx_head)
{
  const char *server_name;
  POUND_CTX *pc;

  if ((server_name = SSL_get_servername (ssl, TLSEXT_NAMETYPE_host_name)) == NULL)
    return SSL_TLSEXT_ERR_NOACK;

  /* logmsg(LOG_DEBUG, "Received SSL SNI Header for servername %s", servername); */

  SSL_set_SSL_CTX (ssl, NULL);
  SLIST_FOREACH (pc, ctx_head, next)
    {
      if (fnmatch (pc->server_name, server_name, 0) == 0)
	{
	  /* logmsg(LOG_DEBUG, "Found cert for %s", servername); */
	  SSL_set_SSL_CTX (ssl, pc->ctx);
	  return SSL_TLSEXT_ERR_OK;
	}
      else if (pc->subjectAltNameCount > 0 && pc->subjectAltNames != NULL)
	{
	  int i;

	  for (i = 0; i < pc->subjectAltNameCount; i++)
	    {
	      if (fnmatch ((char *) pc->subjectAltNames[i], server_name, 0) ==
		  0)
		{
		  SSL_set_SSL_CTX (ssl, pc->ctx);
		  return SSL_TLSEXT_ERR_OK;
		}
	    }
	}
    }

  /* logmsg(LOG_DEBUG, "No match for %s, default used", server_name); */
  SSL_set_SSL_CTX (ssl, SLIST_FIRST (ctx_head)->ctx);
  return SSL_TLSEXT_ERR_OK;
}
#endif

static int
https_parse_client_cert (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  int depth;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "ClientCert may only be used after Cert");
      return CFGPARSER_FAIL;
    }

  if (cfg_assign_int_range (&lst->clnt_check, 0, 3) != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  if (lst->clnt_check > 0 && cfg_assign_int (&depth, NULL) != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  switch (lst->clnt_check)
    {
    case 0:
      /* don't ask */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	SSL_CTX_set_verify (pc->ctx, SSL_VERIFY_NONE, NULL);
      break;

    case 1:
      /* ask but OK if no client certificate */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	{
	  SSL_CTX_set_verify (pc->ctx,
			      SSL_VERIFY_PEER |
			      SSL_VERIFY_CLIENT_ONCE, NULL);
	  SSL_CTX_set_verify_depth (pc->ctx, depth);
	}
      break;

    case 2:
      /* ask and fail if no client certificate */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	{
	  SSL_CTX_set_verify (pc->ctx,
			      SSL_VERIFY_PEER |
			      SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	  SSL_CTX_set_verify_depth (pc->ctx, depth);
	}
      break;

    case 3:
      /* ask but do not verify client certificate */
      SLIST_FOREACH (pc, &lst->ctx_head, next)
	{
	  SSL_CTX_set_verify (pc->ctx,
			      SSL_VERIFY_PEER |
			      SSL_VERIFY_CLIENT_ONCE, verify_OK);
	  SSL_CTX_set_verify_depth (pc->ctx, depth);
	}
      break;
    }
  return CFGPARSER_OK;
}

static int
https_parse_disable (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  return set_proto_opt (&lst->ssl_op_enable);
}

static int
https_parse_ciphers (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "Ciphers may only be used after Cert");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    SSL_CTX_set_cipher_list (pc->ctx, tok->str);

  return CFGPARSER_OK;
}

static int
https_parse_honor_cipher_order (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  int bv;

  if (cfg_assign_bool (&bv, NULL) != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  if (bv)
    {
      lst->ssl_op_enable |= SSL_OP_CIPHER_SERVER_PREFERENCE;
      lst->ssl_op_disable &= ~SSL_OP_CIPHER_SERVER_PREFERENCE;
    }
  else
    {
      lst->ssl_op_disable |= SSL_OP_CIPHER_SERVER_PREFERENCE;
      lst->ssl_op_enable &= ~SSL_OP_CIPHER_SERVER_PREFERENCE;
    }

  return CFGPARSER_OK;
}

static int
https_parse_allow_client_renegotiation (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;

  if (cfg_assign_int_range (&lst->allow_client_reneg, 0, 2) != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  if (lst->allow_client_reneg == 2)
    {
      lst->ssl_op_enable |= SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
      lst->ssl_op_disable &= ~SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
    }
  else
    {
      lst->ssl_op_disable |= SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
      lst->ssl_op_enable &= ~SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
    }

  return CFGPARSER_OK;
}

static int
https_parse_calist (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  STACK_OF (X509_NAME) *cert_names;
  struct token *tok;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "CAList may only be used after Cert");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  if ((cert_names = SSL_load_client_CA_file (tok->str)) == NULL)
    {
      conf_openssl_error (NULL, "SSL_load_client_CA_file");
      return CFGPARSER_FAIL;
    }

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    SSL_CTX_set_client_CA_list (pc->ctx, cert_names);

  return CFGPARSER_OK;
}

static int
https_parse_verifylist (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "VerifyList may only be used after Cert");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    if (SSL_CTX_load_verify_locations (pc->ctx, tok->str, NULL) != 1)
      {
	conf_openssl_error (NULL, "SSL_CTX_load_verify_locations");
	return CFGPARSER_FAIL;
      }

  return CFGPARSER_OK;
}

static int
https_parse_crlist (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  struct token *tok;
  X509_STORE *store;
  X509_LOOKUP *lookup;
  POUND_CTX *pc;

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error ("%s", "CRlist may only be used after Cert");
      return CFGPARSER_FAIL;
    }

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  SLIST_FOREACH (pc, &lst->ctx_head, next)
    {
      store = SSL_CTX_get_cert_store (pc->ctx);
      if ((lookup = X509_STORE_add_lookup (store, X509_LOOKUP_file ())) == NULL)
	{
	  conf_openssl_error (NULL, "X509_STORE_add_lookup");
	  return CFGPARSER_FAIL;
	}

      if (X509_load_crl_file (lookup, tok->str, X509_FILETYPE_PEM) != 1)
	{
	  conf_openssl_error (tok->str, "X509_load_crl_file failed");
	  return CFGPARSER_FAIL;
	}

      X509_STORE_set_flags (store, X509_V_FLAG_CRL_CHECK | X509_V_FLAG_CRL_CHECK_ALL);
    }

  return CFGPARSER_OK;
}

static int
https_parse_nohttps11 (void *call_data, void *section_data)
{
  LISTENER *lst = call_data;
  return cfg_assign_int_range (&lst->noHTTPS11, 0, 2);
}

static CFGPARSER_TABLE https_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },

  {
    .name = "",
    .type = KWT_TABREF,
    .ref = http_common
  },
  {
    .name = "",
    .type = KWT_TABREF,
    .ref = http_deprecated
  },

  {
    .name = "Cert",
    .parser = https_parse_cert
  },
  {
    .name = "ClientCert",
    .parser = https_parse_client_cert
  },
  {
    .name = "Disable",
    .parser = https_parse_disable
  },
  {
    .name = "Ciphers",
    .parser = https_parse_ciphers
  },
  {
    .name = "SSLHonorCipherOrder",
    .parser = https_parse_honor_cipher_order
  },
  {
    .name = "SSLAllowClientRenegotiation",
    .parser = https_parse_allow_client_renegotiation
  },
  {
    .name = "CAlist",
    .parser = https_parse_calist
  },
  {
    .name = "VerifyList",
    .parser = https_parse_verifylist
  },
  {
    .name = "CRLlist",
    .parser = https_parse_crlist
  },
  {
    .name = "NoHTTPS11",
    .parser = https_parse_nohttps11
  },

  { NULL }
};

static int
parse_listen_https (void *call_data, void *section_data)
{
  LISTENER *lst;
  LISTENER_HEAD *list_head = call_data;
  POUND_DEFAULTS *dfl = section_data;
  struct locus_range range;
  POUND_CTX *pc;
  struct stringbuf sb;
  struct token *tok;

  if ((lst = listener_alloc (dfl)) == NULL)
    return CFGPARSER_FAIL;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;
  else if (tok->type == T_STRING)
    {
      if (find_listener_ident (list_head, tok->str))
	{
	  conf_error ("%s", "listener name is not unique");
	  return CFGPARSER_FAIL;
	}
      lst->name = xstrdup (tok->str);
    }
  else
    putback_tkn (tok);

  lst->ssl_op_enable = SSL_OP_ALL;
#ifdef  SSL_OP_NO_COMPRESSION
  lst->ssl_op_enable |= SSL_OP_NO_COMPRESSION;
#endif
  lst->ssl_op_disable =
    SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION | SSL_OP_LEGACY_SERVER_CONNECT |
    SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;

  if (parser_loop (https_parsetab, lst, section_data, &range))
    return CFGPARSER_FAIL;

  if (check_addrinfo (&lst->addr, &range, "ListenHTTPS") != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  lst->locus_str = format_locus_str (&range);

  if (SLIST_EMPTY (&lst->ctx_head))
    {
      conf_error_at_locus_range (&range, "Cert statement is missing");
      return CFGPARSER_FAIL;
    }

#ifdef SSL_CTRL_SET_TLSEXT_SERVERNAME_CB
  if (!SLIST_EMPTY (&lst->ctx_head))
    {
      SSL_CTX *ctx = SLIST_FIRST (&lst->ctx_head)->ctx;
      if (!SSL_CTX_set_tlsext_servername_callback (ctx, SNI_server_name)
	  || !SSL_CTX_set_tlsext_servername_arg (ctx, &lst->ctx_head))
	{
	  conf_openssl_error (NULL, "can't set SNI callback");
	  return CFGPARSER_FAIL;
	}
    }
#endif

  xstringbuf_init (&sb);
  SLIST_FOREACH (pc, &lst->ctx_head, next)
    {
      SSL_CTX_set_app_data (pc->ctx, lst);
      SSL_CTX_set_mode (pc->ctx, SSL_MODE_AUTO_RETRY);
      SSL_CTX_set_options (pc->ctx, lst->ssl_op_enable);
      SSL_CTX_clear_options (pc->ctx, lst->ssl_op_disable);
      stringbuf_reset (&sb);
      stringbuf_printf (&sb, "%d-Pound-%ld", getpid (), random ());
      SSL_CTX_set_session_id_context (pc->ctx, (unsigned char *) sb.base,
				      sb.len);
      POUND_SSL_CTX_init (pc->ctx);
      SSL_CTX_set_info_callback (pc->ctx, SSLINFO_callback);
    }
  stringbuf_free (&sb);

  SLIST_PUSH (list_head, lst, next);
  return CFGPARSER_OK;
}

static int
parse_threads_compat (void *call_data, void *section_data)
{
  int rc;
  unsigned n;

  if ((rc = cfg_assign_unsigned (&n, section_data)) != CFGPARSER_OK)
    return rc;

  worker_min_count = worker_max_count = n;

  return CFGPARSER_OK;
}

static int
parse_control_socket (void *call_data, void *section_data)
{
  struct addrinfo *addr = call_data;
  struct token *tok;
  struct sockaddr_un *sun;
  size_t len;

  /* Get socket address */
  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  len = strlen (tok->str);
  if (len > UNIX_PATH_MAX)
    {
      conf_error_at_locus_range (&tok->locus,
				 "%s", "UNIX path name too long");
      return CFGPARSER_FAIL;
    }

  len += offsetof (struct sockaddr_un, sun_path) + 1;
  sun = xmalloc (len);
  sun->sun_family = AF_UNIX;
  strcpy (sun->sun_path, tok->str);
  unlink_at_exit (sun->sun_path);

  addr->ai_socktype = SOCK_STREAM;
  addr->ai_family = AF_UNIX;
  addr->ai_protocol = 0;
  addr->ai_addr = (struct sockaddr *) sun;
  addr->ai_addrlen = len;

  return CFGPARSER_OK;
}

static CFGPARSER_TABLE control_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "Socket",
    .parser = parse_control_socket,
    .off = offsetof (LISTENER, addr)
  },
  {
    .name = "ChangeOwner",
    .parser = cfg_assign_bool,
    .off = offsetof (LISTENER, chowner)
  },
  {
    .name = "Mode",
    .parser = cfg_assign_mode,
    .off = offsetof (LISTENER, mode)
  },
  { NULL }
};

static int
parse_control_listener (void *call_data, void *section_data)
{
  struct token *tok;
  LISTENER *lst;
  SERVICE *svc;
  BACKEND *be;
  int rc;
  struct locus_range range;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;
  lst = listener_alloc (section_data);
  switch (tok->type)
    {
    case '\n':
      rc = parser_loop (control_parsetab, lst, section_data, &range);
      if (rc == CFGPARSER_OK)
	{
	  if (lst->addr.ai_addrlen == 0)
	    {
	      conf_error_at_locus_range (&range, "%s",
					 "Socket statement is missing");
	      rc = CFGPARSER_FAIL;
	    }
	}
      break;

    case T_STRING:
      range.beg = last_token_locus_range ()->beg;
      putback_tkn (tok);
      rc = parse_control_socket (&lst->addr, section_data);
      range.end = last_token_locus_range ()->end;
      break;

    default:
      conf_error ("expected string or newline, but found %s",
		  token_type_str (tok->type));
      rc = CFGPARSER_FAIL;
    }

  if (rc != CFGPARSER_OK)
    return CFGPARSER_FAIL;

  lst->verb = 1; /* Need PUT and DELETE methods */
  lst->locus_str = format_locus_str (&range);
  /* Register listener in the global listener list */
  SLIST_PUSH (&listeners, lst, next);

  /* Create service; there'll be only one backend so the balancing algorithm
     doesn't really matter. */
  svc = new_service (BALANCER_ALGO_RANDOM);
  lst->locus_str = format_locus_str (&range);

  /* Register service in the listener */
  SLIST_PUSH (&lst->services, svc, next);

  /* Create backend */
  XZALLOC (be);
  be->locus = range;
  be->locus_str = format_locus_str (&range);
  be->be_type = BE_CONTROL;
  be->priority = 1;
  pthread_mutex_init (&be->mut, NULL);
  /* Register backend in service */
  balancer_add_backend (balancer_list_get_normal (&svc->backends), be);
  service_recompute_pri_unlocked (svc, NULL, NULL);
  
  return CFGPARSER_OK;
}

static int
parse_named_backend (void *call_data, void *section_data)
{
  NAMED_BACKEND_TABLE *tab = call_data;
  struct token *tok;
  BACKEND *be;
  struct locus_range range;
  NAMED_BACKEND *olddef;
  char *name;

  range.beg = last_token_locus_range ()->beg;

  if ((tok = gettkn_expect (T_STRING)) == NULL)
    return CFGPARSER_FAIL;

  name = xstrdup (tok->str);

  be = parse_backend_internal (backend_parsetab, section_data, NULL);
  if (!be)
    return CFGPARSER_FAIL;
  range.end = last_token_locus_range ()->end;

  olddef = named_backend_insert (tab, name, &range, be);
  free (name);
  pthread_mutex_destroy (&be->mut);
  free (be->locus_str);
  free (be);
  // FIXME: free address on failure only.

  if (olddef)
    {
      conf_error_at_locus_range (&range, "redefinition of named backend %s",
				 olddef->name);
      conf_error_at_locus_range (&olddef->locus,
				 "original definition was here");
      return CFGPARSER_FAIL;
    }

  return CFGPARSER_OK;
}

static int
parse_combine_headers (void *call_data, void *section_data)
{
  struct token *tok;

  if ((tok = gettkn_any ()) == NULL)
    return CFGPARSER_FAIL;

  if (tok->type != '\n')
    {
      conf_error ("expected newline, but found %s", token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }

  for (;;)
    {
      int rc;
      if ((tok = gettkn_any ()) == NULL)
	return CFGPARSER_FAIL;
      if (tok->type == '\n')
	continue;
      if (tok->type == T_IDENT)
	{
	  if (strcasecmp (tok->str, "end") == 0)
	    break;
	  if (strcasecmp (tok->str, "include") == 0)
	    {
	      if ((rc = cfg_parse_include (NULL, NULL)) == CFGPARSER_FAIL)
		return rc;
	      continue;
	    }
	  conf_error ("expected quoted string, \"Include\", or \"End\", but found %s",
		      token_type_str (tok->type));
	  return CFGPARSER_FAIL;
	}
      if (tok->type == T_STRING)
	combinable_header_add (tok->str);
      else
	{
	  conf_error ("expected quoted string, \"Include\", or \"End\", but found %s",
		      token_type_str (tok->type));
	  return CFGPARSER_FAIL;
	}
    }
  return CFGPARSER_OK;
}

static struct kwtab regex_type_table[] = {
  { "posix", GENPAT_POSIX },
#ifdef HAVE_LIBPCRE
  { "pcre",  GENPAT_PCRE },
  { "perl",  GENPAT_PCRE },
#endif
  { NULL }
};

static int
assign_regex_type (void *call_data, void *section_data)
{
  return cfg_assign_int_enum (call_data, gettkn_expect (T_IDENT),
			      regex_type_table,
			      "regex type");
}

static int
read_resolv_conf (void *call_data, void *section_data)
{
  char **pstr = call_data;

  if (*pstr)
    {
      conf_error ("%s", "ConfigFile statement overrides prior ConfigText");
      free (*pstr);
      *pstr = NULL;
    }
  return cfg_assign_string_from_file (pstr, section_data);
}
  
static int
read_resolv_text (void *call_data, void *section_data)
{
  char **pstr = call_data;
  char *str;
  struct token *tok;
  
  if (*pstr)
    {
      conf_error ("%s", "ConfigText statement overrides prior ConfigFile");
      free (*pstr);
      *pstr = NULL;
    }

  if ((tok = gettkn_any ()) == NULL)
    {
      conf_error ("%s", "unexpected end of file");
      return CFGPARSER_FAIL;
    }
  if (tok->type != '\n')
    {
      conf_error ("expected newline, but found %s",
		  token_type_str (tok->type));
      return CFGPARSER_FAIL;
    }
      
  if (cfg_read_to_end (cur_input, &str) == EOF)
    return CFGPARSER_FAIL;
  *pstr = xstrdup (str);
  return CFGPARSER_OK_NONL;
}

static CFGPARSER_TABLE resolver_parsetab[] = {
  {
    .name = "End",
    .parser = cfg_parse_end
  },
  {
    .name = "ConfigFile",
    .parser = read_resolv_conf,
  },
  {
    .name = "ConfigText",
    .parser = read_resolv_text,
  },    
  {
    .name = "Debug",
    .parser = cfg_assign_bool,
    .off = offsetof (struct resolver_config, debug)
  },
  {
    .name = "CNAMEChain",
    .parser = cfg_assign_unsigned,
    .off = offsetof (struct resolver_config, max_cname_chain)
  },
  {
    .name = "RetryInterval",
    .parser = cfg_assign_timeout,
    .off = offsetof (struct resolver_config, retry_interval)
  },
  { NULL }
};

static int
parse_resolver (void *call_data, void *section_data)
{
  POUND_DEFAULTS *dfl = section_data;
  struct locus_range range;
  int rc = parser_loop (resolver_parsetab, &dfl->resolver, dfl, &range);
#ifndef ENABLE_DYNAMIC_BACKENDS
  if (rc == CFGPARSER_OK)
    conf_error_at_locus_range (&range, "%s",
			       "section ignored: "
			       "pound compiled without support "
			       "for dynamic backends");
#endif
  return rc;
}

static CFGPARSER_TABLE top_level_parsetab[] = {
  {
    .name = "IncludeDir",
    .parser = cfg_parse_includedir
  },
  {
    .name = "User",
    .parser = cfg_assign_string,
    .data = &user
  },
  {
    .name = "Group",
    .parser = cfg_assign_string,
    .data = &group
  },
  {
    .name = "RootJail",
    .parser = cfg_assign_string,
    .data = &root_jail
  },
  {
    .name = "Daemon",
    .parser = cfg_assign_bool,
    .data = &daemonize
  },
  {
    .name = "Supervisor",
    .parser = cfg_assign_bool,
    .data = &enable_supervisor
  },
  {
    .name = "WorkerMinCount",
    .parser = cfg_assign_unsigned,
    .data = &worker_min_count
  },
  {
    .name = "WorkerMaxCount",
    .parser = cfg_assign_unsigned,
    .data = &worker_max_count
  },
  {
    .name = "Threads",
    .parser = parse_threads_compat
  },
  {
    .name = "WorkerIdleTimeout",
    .parser = cfg_assign_timeout,
    .data = &worker_idle_timeout
  },
  {
    .name = "Grace",
    .parser = cfg_assign_timeout,
    .data = &grace
  },
  {
    .name = "LogFacility",
    .parser = cfg_assign_log_facility,
    .off = offsetof (POUND_DEFAULTS, facility)
  },
  {
    .name = "LogLevel",
    .parser = parse_log_level,
    .off = offsetof (POUND_DEFAULTS, log_level)
  },
  {
    .name = "LogFormat",
    .parser = parse_log_format
  },
  {
    .name = "LogTag",
    .parser = cfg_assign_string,
    .data = &syslog_tag
  },
  {
    .name = "Alive",
    .parser = cfg_assign_timeout,
    .data = &alive_to
  },
  {
    .name = "Client",
    .parser = cfg_assign_timeout,
    .off = offsetof (POUND_DEFAULTS, clnt_to)
  },
  {
    .name = "TimeOut",
    .parser = cfg_assign_timeout,
    .off = offsetof (POUND_DEFAULTS, be_to)
  },
  {
    .name = "WSTimeOut",
    .parser = cfg_assign_timeout,
    .off = offsetof (POUND_DEFAULTS, ws_to)
  },
  {
    .name = "ConnTO",
    .parser = cfg_assign_timeout,
    .off = offsetof (POUND_DEFAULTS, be_connto)
  },
  {
    .name = "Balancer",
    .parser = parse_balancer,
    .off = offsetof (POUND_DEFAULTS, balancer_algo)
  },
  {
    .name = "HeaderOption",
    .parser = parse_header_options,
    .off = offsetof (POUND_DEFAULTS, header_options)
  },
  {
    .name = "ECDHCurve",
    .parser = parse_ECDHCurve
  },
  {
    .name = "SSLEngine",
    .parser = parse_SSLEngine
  },
  {
    .name = "Control",
    .parser = parse_control_listener
  },
  {
    .name = "Anonymise",
    .parser = cfg_int_set_one,
    .data = &anonymise
  },
  {
    .name = "Anonymize",
    .type = KWT_ALIAS
  },
  {
    .name = "Service",
    .parser = parse_service,
    .data = &services
  },
  {
    .name = "Backend",
    .parser = parse_named_backend,
    .off = offsetof (POUND_DEFAULTS, named_backend_table)
  },
  {
    .name = "ListenHTTP",
    .parser = parse_listen_http,
    .data = &listeners
  },
  {
    .name = "ListenHTTPS",
    .parser = parse_listen_https,
    .data = &listeners
  },
  {
    .name = "ACL",
    .parser = parse_named_acl
  },
  {
    .name = "PidFile",
    .parser = cfg_assign_string,
    .data = &pid_name
  },
  {
    .name = "BackendStats",
    .parser = cfg_assign_bool,
    .data = &enable_backend_stats
  },
  {
    .name = "ForwardedHeader",
    .parser = cfg_assign_string,
    .data = &forwarded_header
  },
  {
    .name = "TrustedIP",
    .parser = assign_acl,
    .data = &trusted_ips
  },
  {
    .name = "CombineHeaders",
    .parser = parse_combine_headers
  },
  {
    .name = "RegexType",
    .parser = assign_regex_type,
    .off = offsetof (POUND_DEFAULTS, re_type),
  },
  {
    .name = "Resolver",
    .parser = parse_resolver
  },
  /* Backward compatibility. */
  {
    .name = "IgnoreCase",
    .parser = cfg_assign_bool,
    .off = offsetof (POUND_DEFAULTS, ignore_case),
    .deprecated = 1,
    .message = "use the -icase matching directive flag to request case-insensitive comparison"
  },

  { NULL }
};

static int
str_is_ipv4 (const char *addr)
{
  int c;
  int dot_count;
  int digit_count;

  dot_count = 0;
  digit_count = 0;
  for (; (c = *addr) != 0; addr++)
    {
      if (c == '.')
	{
	  if (++dot_count > 4)
	    return 0;
	  digit_count = 0;
	}
      else if (!(isdigit (c) && ++digit_count <= 3))
	return 0;
    }

  return dot_count == 3;
}

static int
str_is_ipv6 (const char *addr)
{
  int c;
  int col_count = 0; /* Number of colons */
  int dcol = 0;      /* Did we encounter a double-colon? */
  int dig_count = 0; /* Number of digits in the last group */

  for (; (c = *addr) != 0; addr++)
    {
      if (!isascii (c))
	return 0;
      else if (isxdigit (c))
	{
	  if (++dig_count > 4)
	    return 0;
	}
      else if (c == ':')
	{
	  if (col_count && dig_count == 0 && ++dcol > 1)
	    return 0;
	  if (++col_count > 7)
	    return 0;
	  dig_count = 0;
	}
      else
	return 0;
    }
  return col_count == 7 || dcol;
}

static int
str_is_ip (const char *addr)
{
  int c;
  int dot = 0;
  for (; (c = *addr) != 0 && isascii (c); addr++)
    {
      if (!isascii (c))
	break;
      else if (isxdigit (c))
	return str_is_ipv6 (addr);
      else if (c == dot)
	return str_is_ipv4 (addr);
      else if (isdigit (c))
	dot = '.';
      else
	break;
    }
  return 0;
}

void
backend_matrix_to_regular (struct be_matrix *mtx, struct addrinfo *addr,
			   struct be_regular *reg)
{
  memset (reg, 0, sizeof (*reg));
  reg->addr = *addr;

  switch (reg->addr.ai_family)
    {
    case AF_INET:
      ((struct sockaddr_in *)reg->addr.ai_addr)->sin_port = mtx->port;
      break;

    case AF_INET6:
      ((struct sockaddr_in6 *)reg->addr.ai_addr)->sin6_port = mtx->port;
      break;
    }

  reg->alive = 1;
  reg->to = mtx->to;
  reg->conn_to = mtx->conn_to;
  reg->ws_to = mtx->ws_to;
  reg->ctx = mtx->ctx;
  reg->servername = mtx->servername;
}

static int
backend_resolve (BACKEND *be)
{
  struct addrinfo addr;
  struct be_regular reg;
  char *hostname = be->v.mtx.hostname;

  if (resolve_address (hostname, &be->locus, be->v.mtx.family, &addr))
    return -1;

  backend_matrix_to_regular (&be->v.mtx, &addr, &reg);
  free (hostname);
  be->v.reg = reg;
  be->be_type = BE_REGULAR;
  be->refcount = 1;
  return 0;
}

static int
backend_finalize (BACKEND *be, void *data)
{
  if (be->be_type == BE_BACKEND_REF)
    {
      NAMED_BACKEND_TABLE *tab = data;
      NAMED_BACKEND *nb;

      nb = named_backend_retrieve (tab, be->v.be_name);
      if (!nb)
	{
	  logmsg (LOG_ERR, "%s: named backend %s is not declared",
		  be->locus_str, be->v.be_name);
	  return -1;
	}
      free (be->v.be_name);
      be->be_type = BE_MATRIX;
      be->v.mtx = nb->bemtx;
      /* Hostname will be freed after resolving backend to be_regular.
	 FIXME: use STRING_REF? */
      be->v.mtx.hostname = xstrdup (be->v.mtx.hostname);
      if (be->priority == -1)
	be->priority = nb->priority;
      if (be->disabled == -1)
	be->disabled = nb->disabled;
    }

  if (be->be_type == BE_MATRIX)
    {
      if (!be->v.mtx.hostname)
	{
	  conf_error_at_locus_range (&be->locus, "%s",
				     "Backend missing Address declaration");
	  return -1;
	}

      if (be->v.mtx.hostname[0] == '/' || str_is_ip (be->v.mtx.hostname))
	be->v.mtx.resolve_mode = bres_immediate;

      if (be->v.mtx.port == 0)
	{
	  be->v.mtx.port = htons (be->v.mtx.ctx == NULL ? 80 : 443);
	}
      else if (be->v.mtx.hostname[0] == '/')
	{
	  conf_error_at_locus_range (&be->locus,
				     "Port is not applicable to this address family");
	  return -1;
	}

      if (be->v.mtx.resolve_mode == bres_immediate)
	{
	  if (backend_resolve (be))
	    return -1;
	}
      else
	{
#ifdef ENABLE_DYNAMIC_BACKENDS
	  if (feature_is_set (FEATURE_DNS))
	    {
	      backend_matrix_init (be);
	    }
	  else
	    {
	      conf_error_at_locus_range (&be->locus,
					 "Dynamic backend creation is not "
					 "available: disabled by -Wno-dns");
	      return 1;
	    }
#else
	  conf_error_at_locus_range (&be->locus,
				     "Dynamic backend creation is not "
				     "available: pound compiled without "
				     "support for dynamic backends");
	  return 1;

#endif
	}
    }
  return 0;
}

/*
 * Fix-up password file structures for use in restricted chroot
 * environment.
 */
static int
cond_pass_file_fixup (SERVICE_COND *cond)
{
  int rc = 0;

  switch (cond->type)
    {
    case COND_BASIC_AUTH:
      if (cond->pwfile.filename[0] == '/')
	{
	  if (root_jail)
	    {
	      /* Split file name into directory and base name, */
	      char *p = strrchr (cond->pwfile.filename, '/');
	      if (p != NULL)
		{
		  char *dir = cond->pwfile.filename;
		  *p++ = 0;
		  cond->pwfile.filename = xstrdup (p);
		  if ((cond->pwfile.wd = workdir_get (dir)) == NULL)
		    {
		      conf_error_at_locus_range (&cond->pwfile.locus,
						 "can't open directory %s: %s",
						 dir,
						 strerror (errno));
		      free (dir);
		      rc = -1;
		      break;
		    }
		  free (dir);
		}
	    }
	}
      else
	{
	  WORKDIR *wd = get_include_wd_at_locus_range (&cond->pwfile.locus);
	  if (!wd)
	    {
	      rc = -1;
	      break;
	    }
	  cond->pwfile.wd = workdir_ref (wd);
	}
      break;

    case COND_BOOL:
      {
	SERVICE_COND *subcond;
	SLIST_FOREACH (subcond, &cond->bool.head, next)
	  {
	    if ((rc = cond_pass_file_fixup (subcond)) != 0)
	      break;
	  }
      }
      break;

    default:
      break;
    }
  return rc;
}

static int
rule_pass_file_fixup (REWRITE_RULE *rule)
{
  int rc = 0;
  do
    {
      if ((rc = cond_pass_file_fixup (&rule->cond)) != 0)
	break;
    }
  while ((rule = rule->iffalse) != NULL);
  return rc;
}

static int
pass_file_fixup (REWRITE_RULE_HEAD *head)
{
  REWRITE_RULE *rule;
  int rc = 0;

  SLIST_FOREACH (rule, head, next)
    {
      if ((rc = rule_pass_file_fixup (rule)) != 0)
	break;
    }
  return rc;
}

static int
service_pass_file_fixup (SERVICE *svc, void *data)
{
  if (cond_pass_file_fixup (&svc->cond))
    return -1;
  return pass_file_fixup (&svc->rewrite[REWRITE_REQUEST]);
}

static int
listener_pass_file_fixup (LISTENER *lstn, void *data)
{
  return pass_file_fixup (&lstn->rewrite[REWRITE_REQUEST]);
}

int
parse_config_file (char const *file, int nosyslog)
{
  int res = -1;
  POUND_DEFAULTS pound_defaults = {
    .log_level = 1,
    .facility = LOG_DAEMON,
    .clnt_to = 10,
    .be_to = 15,
    .ws_to = 600,
    .be_connto = 15,
    .ignore_case = 0,
    .re_type = GENPAT_POSIX,
    .header_options = HDROPT_FORWARDED_HEADERS | HDROPT_SSL_HEADERS,
    .balancer_algo = BALANCER_ALGO_RANDOM,
    .resolver = RESOLVER_CONFIG_INITIALIZER
  };

  named_backend_table_init (&pound_defaults.named_backend_table);
  compile_canned_formats ();

  if (cfgparser_open (file))
    return -1;
  
  res = parser_loop (top_level_parsetab, &pound_defaults, &pound_defaults, NULL);
  if (res == 0)
    {
      if (cur_input)
	return -1;

#ifdef ENABLE_DYNAMIC_BACKENDS
      resolver_set_config (&pound_defaults.resolver);
#endif
      if (foreach_backend (backend_finalize,
			   &pound_defaults.named_backend_table))
	return -1;
      if (worker_min_count > worker_max_count)
	abend ("WorkerMinCount is greater than WorkerMaxCount");
      if (!nosyslog)
	log_facility = pound_defaults.facility;

      if (foreach_listener (listener_pass_file_fixup, NULL)
	  || foreach_service (service_pass_file_fixup, NULL))
	return -1;
    }
  named_backend_table_free (&pound_defaults.named_backend_table);
  cfgparser_finish (root_jail || daemonize);
  return res;
}

enum
  {
    F_OFF,
    F_ON,
    F_DFL
  };

struct pound_feature
{
  char *name;
  char *descr;
  int enabled;
  void (*setfn) (int, char const *);
};

static void
set_include_dir (int enabled, char const *val)
{
  if (enabled)
    {
      struct stat st;
      if (val && (*val == 0 || strcmp (val, ".") == 0))
	val = NULL;
      else if (stat (val, &st))
	{
	  logmsg (LOG_ERR, "include-dir: can't stat %s: %s", val, strerror (errno));
	  exit (1);
	}
      else if (!S_ISDIR (st.st_mode))
	{
	  logmsg (LOG_ERR, "include-dir: %s is not a directory", val);
	  exit (1);
	}
      include_dir = val;
    }
  else
    include_dir = NULL;
}

static struct pound_feature feature[] = {
  [FEATURE_DNS] = {
    .name = "dns",
    .descr = "resolve host names found in configuration file (default)",
    .enabled = F_ON
  },
  [FEATURE_INCLUDE_DIR] = {
    .name = "include-dir",
    .descr = "include file directory",
    .enabled = F_DFL,
    .setfn = set_include_dir
  },
  [FEATURE_WARN_DEPRECATED] = {
    .name = "warn-deprecated",
    .descr = "warn if deprecated configuration statements are used (default)",
    .enabled = F_DFL,
  },
  { NULL }
};

int
feature_is_set (int f)
{
  return feature[f].enabled;
}

static int
feature_set (char const *name)
{
  int i, enabled = F_ON;
  size_t len;
  char *val;

  if ((val = strchr (name, '=')) != NULL)
    {
      len = val - name;
      val++;
    }
  else
    len = strlen (name);

  if (val == NULL && strncmp (name, "no-", 3) == 0)
    {
      name += 3;
      len -= 3;
      enabled = F_OFF;
    }

  if (*name)
    {
      for (i = 0; feature[i].name; i++)
	{
	  if (strlen (feature[i].name) == len &&
	      memcmp (feature[i].name, name, len) == 0)
	    {
	      if (feature[i].setfn)
		feature[i].setfn (enabled, val);
	      else if (val)
		break;
	      feature[i].enabled = enabled;
	      return 0;
	    }
	}
    }
  return -1;
}

struct string_value pound_settings[] = {
  { "Configuration file",  STRING_CONSTANT, { .s_const = POUND_CONF } },
  { "Include directory",   STRING_CONSTANT, { .s_const = SYSCONFDIR } },
  { "PID file",   STRING_CONSTANT,  { .s_const = POUND_PID } },
  { "Buffer size",STRING_INT, { .s_int = MAXBUF } },
  { "Regex types", STRING_CONSTANT, { .s_const = "POSIX"
#if HAVE_LIBPCRE == 1
				       ", PCRE"
#elif HAVE_LIBPCRE == 2
				       ", PCRE2"
#endif
    }
  },
  { "Dynamic backends", STRING_CONSTANT, { .s_const =
#if ENABLE_DYNAMIC_BACKENDS
					  "enabled"
#else
					  "disabled"
#endif
    }
  },
#if ! SET_DH_AUTO
  { "DH bits",         STRING_INT, { .s_int = DH_LEN } },
  { "RSA regeneration interval", STRING_INT, { .s_int = T_RSA_KEYS } },
#endif
  { NULL }
};

void
print_help (void)
{
  int i;

  printf ("usage: %s [-FVcehv] [-W [no-]FEATURE] [-f FILE] [-p FILE]\n", progname);
  printf ("HTTP/HTTPS reverse-proxy and load-balancer\n");
  printf ("\nOptions are:\n\n");
  printf ("   -c               check configuration file syntax and exit\n");
  printf ("   -e               print errors on stderr (implies -F)\n");
  printf ("   -F               remain in foreground after startup\n");
  printf ("   -f FILE          read configuration from FILE\n");
  printf ("                    (default: %s)\n", POUND_CONF);
  printf ("   -p FILE          write PID to FILE\n");
  printf ("                    (default: %s)\n", POUND_PID);
  printf ("   -V               print program version, compilation settings, and exit\n");
  printf ("   -v               print log messages to stdout/stderr during startup\n");
  printf ("   -W [no-]FEATURE  enable or disable optional feature\n");
  printf ("\n");
  printf ("FEATUREs are:\n");
  for (i = 0; feature[i].name; i++)
    printf ("   %-16s %s\n", feature[i].name, feature[i].descr);
  printf ("\n");
  printf ("Report bugs and suggestions to <%s>\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
  printf ("%s home page: <%s>\n", PACKAGE_NAME, PACKAGE_URL);
#endif
}

void
config_parse (int argc, char **argv)
{
  int c;
  int check_only = 0;
  char *conf_name = POUND_CONF;
  char *pid_file_option = NULL;
  int foreground_option = 0;
  int stderr_option = 0;

  set_progname (argv[0]);

  while ((c = getopt (argc, argv, "ceFf:hp:VvW:")) > 0)
    switch (c)
      {
      case 'c':
	check_only = 1;
	break;

      case 'e':
	stderr_option = foreground_option = 1;
	break;

      case 'F':
	foreground_option = 1;
	break;

      case 'f':
	conf_name = optarg;
	break;

      case 'h':
	print_help ();
	exit (0);

      case 'p':
	pid_file_option = optarg;
	break;

      case 'V':
	print_version (pound_settings);
	exit (0);

      case 'v':
	print_log = 1;
	break;

      case 'W':
	if (feature_set (optarg))
	  {
	    logmsg (LOG_ERR, "invalid feature name: %s", optarg);
	    exit (1);
	  }
	break;

      default:
	exit (1);
      }

  if (optind < argc)
    {
      logmsg (LOG_ERR, "unknown extra arguments (%s...)", argv[optind]);
      exit (1);
    }

  if (parse_config_file (conf_name, stderr_option))
    exit (1);

  if (check_only)
    {
      logmsg (LOG_INFO, "Config file %s is OK", conf_name);
      exit (0);
    }

  if (SLIST_EMPTY (&listeners))
    abend ("no listeners defined");

  if (pid_file_option)
    pid_name = pid_file_option;

  if (foreground_option)
    daemonize = 0;

  if (daemonize)
    {
      if (log_facility == -1)
	log_facility = LOG_DAEMON;
    }
}
