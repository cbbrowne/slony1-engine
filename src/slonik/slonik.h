/*-------------------------------------------------------------------------
 * slonik.h
 *
 *	Definitions for slonik
 *
 *	Copyright (c) 2003-2004, PostgreSQL Global Development Group
 *	Author: Jan Wieck, Afilias USA INC.
 *
 *	$Id: slonik.h,v 1.1 2004-03-10 21:27:32 wieck Exp $
 *-------------------------------------------------------------------------
 */


typedef struct SlonikScript_s					SlonikScript;
typedef struct SlonikAdmInfo_s					SlonikAdmInfo;
typedef struct SlonikStmt_s						SlonikStmt;
typedef struct SlonikStmt_try_s					SlonikStmt_try;
typedef struct SlonikStmt_echo_s				SlonikStmt_echo;
typedef struct SlonikStmt_exit_s				SlonikStmt_exit;
typedef struct SlonikStmt_init_cluster_s		SlonikStmt_init_cluster;
typedef struct SlonikStmt_store_node_s			SlonikStmt_store_node;
typedef struct SlonikStmt_store_path_s			SlonikStmt_store_path;
typedef struct SlonikStmt_store_listen_s		SlonikStmt_store_listen;
typedef struct SlonikStmt_drop_listen_s			SlonikStmt_drop_listen;
typedef struct SlonikStmt_create_set_s			SlonikStmt_create_set;
typedef struct SlonikStmt_set_add_table_s		SlonikStmt_set_add_table;
typedef struct SlonikStmt_table_add_key_s		SlonikStmt_table_add_key;
typedef struct SlonikStmt_subscribe_set_s		SlonikStmt_subscribe_set;

typedef enum {
	STMT_TRY = 1,
	STMT_ECHO,
	STMT_EXIT,
	STMT_INIT_CLUSTER,
	STMT_STORE_NODE,
	STMT_STORE_PATH,
	STMT_STORE_LISTEN,
	STMT_DROP_LISTEN,
	STMT_CREATE_SET,
	STMT_SET_ADD_TABLE,
	STMT_TABLE_ADD_KEY,
	STMT_SUBSCRIBE_SET,
	STMT_ERROR
} Slonik_stmttype;
	
struct SlonikScript_s {
	char			   *clustername;
	char			   *filename;

	SlonikAdmInfo	   *adminfo_list;

	SlonikStmt		   *script_stmts;
};


struct SlonikAdmInfo_s {
	int					no_id;
	char			   *stmt_filename;
	int					stmt_lno;
	char			   *conninfo;
	PGconn			   *dbconn;
	int					version_major;
	int					version_minor;
	int					nodeid_checked;
	int					have_xact;
	SlonikScript	   *script;
	SlonikAdmInfo	   *next;
};


struct SlonikStmt_s {
	Slonik_stmttype		stmt_type;
	char			   *stmt_filename;
	int					stmt_lno;
	SlonikScript	   *script;
	SlonikStmt		   *next;
};


struct SlonikStmt_try_s {
	SlonikStmt			hdr;
	SlonikStmt		   *try_block;
	SlonikStmt		   *error_block;
	SlonikStmt		   *success_block;
};


struct SlonikStmt_echo_s {
	SlonikStmt			hdr;
	char			   *str;
};


struct SlonikStmt_exit_s {
	SlonikStmt			hdr;
	int					exitcode;
};


struct SlonikStmt_init_cluster_s {
	SlonikStmt			hdr;
	int					no_id;
	char			   *no_comment;
};


struct SlonikStmt_store_node_s {
	SlonikStmt			hdr;
	int					no_id;
	char			   *no_comment;
	int					ev_origin;
};


struct SlonikStmt_store_path_s {
	SlonikStmt			hdr;
	int					pa_server;
	int					pa_client;
	char			   *pa_conninfo;
	int					pa_connretry;
};


struct SlonikStmt_store_listen_s {
	SlonikStmt			hdr;
	int					li_origin;
	int					li_receiver;
	int					li_provider;
};


struct SlonikStmt_drop_listen_s {
	SlonikStmt			hdr;
	int					li_origin;
	int					li_receiver;
	int					li_provider;
};


struct SlonikStmt_create_set_s {
	SlonikStmt			hdr;
	int					set_id;
	int					set_origin;
	char			   *set_comment;
};


struct SlonikStmt_set_add_table_s {
	SlonikStmt			hdr;
	int					set_id;
	int					set_origin;
	int					tab_id;
	int					use_serial;
	char			   *use_key;
	char			   *tab_fqname;
	char			   *tab_comment;
};


struct SlonikStmt_table_add_key_s {
	SlonikStmt			hdr;
	int					no_id;
	char			   *tab_fqname;
};


struct SlonikStmt_subscribe_set_s {
	SlonikStmt			hdr;
	int					sub_setid;
	int					sub_provider;
	int					sub_receiver;
	int					sub_forward;
};




extern SlonikScript	   *parser_script;
extern int				parser_errors;




/* ----------
 * SlonDString
 * ----------
 */
#define		SLON_DSTRING_SIZE_INIT	256
#define		SLON_DSTRING_SIZE_INC	2

typedef struct
{
	size_t		n_alloc;
	size_t		n_used;
	char	   *data;
} SlonDString;

#define		dstring_init(__ds) \
do { \
	(__ds)->n_alloc = SLON_DSTRING_SIZE_INIT; \
	(__ds)->n_used = 0; \
	(__ds)->data = malloc(SLON_DSTRING_SIZE_INIT); \
	if ((__ds)->data == NULL) { \
		perror("dstring_init: malloc()"); \
		exit(-1); \
	} \
} while (0)
#define		dstring_reset(__ds) \
do { \
	(__ds)->n_used = 0; \
	(__ds)->data[0] = '\0'; \
} while (0)
#define		dstring_free(__ds) \
do { \
	free((__ds)->data); \
	(__ds)->n_used = 0; \
	(__ds)->data = NULL; \
} while (0)
#define		dstring_nappend(__ds,__s,__n) \
do { \
	if ((__ds)->n_used + (__n) >= (__ds)->n_alloc)  \
	{ \
		while ((__ds)->n_used + (__n) >= (__ds)->n_alloc) \
			(__ds)->n_alloc *= SLON_DSTRING_SIZE_INC; \
		(__ds)->data = realloc((__ds)->data, (__ds)->n_alloc); \
		if ((__ds)->data == NULL) \
		{ \
			perror("dstring_nappend: realloc()"); \
			exit(-1); \
		} \
	} \
	memcpy(&((__ds)->data[(__ds)->n_used]), (__s), (__n)); \
	(__ds)->n_used += (__n); \
} while (0)
#define		dstring_append(___ds,___s) \
do { \
	register int ___n = strlen((___s)); \
	dstring_nappend((___ds),(___s),___n); \
} while (0)
#define		dstring_addchar(__ds,__c) \
do { \
	if ((__ds)->n_used + 1 >= (__ds)->n_alloc)  \
	{ \
		(__ds)->n_alloc *= SLON_DSTRING_SIZE_INC; \
		(__ds)->data = realloc((__ds)->data, (__ds)->n_alloc); \
		if ((__ds)->data == NULL) \
		{ \
			perror("dstring_append: realloc()"); \
			exit(-1); \
		} \
	} \
	(__ds)->data[(__ds)->n_used++] = (__c); \
} while (0)
#define		dstring_terminate(__ds) \
do { \
	(__ds)->data[(__ds)->n_used] = '\0'; \
} while (0)
#define		dstring_data(__ds)	((__ds)->data)


/*
 * Globals in slonik.c
 */
extern int		parser_errors;
extern char	   *current_file;

extern int		slonik_init_cluster(SlonikStmt_init_cluster *stmt);
extern int		slonik_store_node(SlonikStmt_store_node *stmt);
extern int		slonik_store_path(SlonikStmt_store_path *stmt);
extern int		slonik_store_listen(SlonikStmt_store_listen *stmt);
extern int		slonik_drop_listen(SlonikStmt_drop_listen *stmt);
extern int		slonik_create_set(SlonikStmt_create_set *stmt);
extern int		slonik_set_add_table(SlonikStmt_set_add_table *stmt);
extern int		slonik_table_add_key(SlonikStmt_table_add_key *stmt);
extern int		slonik_subscribe_set(SlonikStmt_subscribe_set *stmt);


/*
 * Globals in dbutil.c
 */
extern int		db_notice_silent;
extern int		db_notice_lno;

void			db_notice_recv(void *arg, const PGresult *res);
int				db_connect(SlonikStmt *stmt, SlonikAdmInfo *adminfo);
int				db_disconnect(SlonikStmt *stmt, SlonikAdmInfo *adminfo);

int				db_exec_command(SlonikStmt *stmt, SlonikAdmInfo *adminfo,
									SlonDString *query);
PGresult	   *db_exec_select(SlonikStmt *stmt, SlonikAdmInfo *adminfo,
									SlonDString *query);
int				db_get_version(SlonikStmt *stmt, SlonikAdmInfo *adminfo,
									int *major, int *minor);
int				db_check_namespace(SlonikStmt *stmt, SlonikAdmInfo *adminfo,
									char *clustername);
int				db_get_nodeid(SlonikStmt *stmt, SlonikAdmInfo *adminfo);
int				db_begin_xact(SlonikStmt *stmt, SlonikAdmInfo *adminfo);
int				db_commit_xact(SlonikStmt *stmt, SlonikAdmInfo *adminfo);
int				db_rollback_xact(SlonikStmt *stmt, SlonikAdmInfo *adminfo);

int				slon_mkquery(SlonDString *dsp, char *fmt, ...);
int				slon_appendquery(SlonDString *dsp, char *fmt, ...);


/*
 * Parser related globals
 */
extern int		yylineno;
extern char	   *yytext;
extern FILE	   *yyin;

extern void		yyerror(const char *str);
extern int		yyparse(void);
extern int		yylex(void);

