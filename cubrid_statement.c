/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2010 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Yifan Fan <fanyifan@nhn.com>                                 |
  +----------------------------------------------------------------------+
*/

/************************************************************************
* IMPORTED SYSTEM HEADER FILES
************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "ext/pdo/php_pdo.h"
#include "ext/pdo/php_pdo_driver.h"

#include "zend_exceptions.h"

/************************************************************************
* OTHER IMPORTED HEADER FILES
************************************************************************/

#include "php_pdo_cubrid.h"
#include "php_pdo_cubrid_int.h"
#include <cas_cci.h>

/************************************************************************
* PRIVATE DEFINITIONS
************************************************************************/

#define CUBRID_LOB_READ_BUF_SIZE    8192

/* Maximum length for the Cubrid data types */
#define MAX_CUBRID_CHAR_LEN   1073741823
#define MAX_LEN_INTEGER	      (10 + 1)
#define MAX_LEN_SMALLINT      (5 + 1)
#define MAX_LEN_BIGINT	      (19 + 1)
#define MAX_LEN_FLOAT	      (14 + 1)
#define MAX_LEN_DOUBLE	      (28 + 1)
#define MAX_LEN_MONETARY      (28 + 2)
#define MAX_LEN_DATE	      10
#define MAX_LEN_TIME	      8
#define MAX_LEN_TIMESTAMP     23
#define MAX_LEN_DATETIME      MAX_LEN_TIMESTAMP
#define MAX_LEN_OBJECT	      MAX_CUBRID_CHAR_LEN
#define MAX_LEN_SET	          MAX_CUBRID_CHAR_LEN
#define MAX_LEN_MULTISET      MAX_CUBRID_CHAR_LEN
#define MAX_LEN_SEQUENCE      MAX_CUBRID_CHAR_LEN
#define MAX_LEN_LOB			  MAX_CUBRID_CHAR_LEN

/************************************************************************
* PRIVATE TYPE DEFINITIONS
************************************************************************/

typedef struct
{
    char *type_name;
    T_CCI_U_TYPE cubrid_u_type;
    int len;
} DB_TYPE_INFO;

/* Define Cubrid supported date types */
static const DB_TYPE_INFO db_type_info[] = {
    {"NULL", CCI_U_TYPE_NULL, 0},
    {"UNKNOWN", CCI_U_TYPE_UNKNOWN, MAX_LEN_OBJECT},

    {"CHAR", CCI_U_TYPE_CHAR, -1},
    {"STRING", CCI_U_TYPE_STRING, -1},
    {"NCHAR", CCI_U_TYPE_NCHAR, -1},
    {"VARNCHAR", CCI_U_TYPE_VARNCHAR, -1},

    {"BIT", CCI_U_TYPE_BIT, -1},
    {"VARBIT", CCI_U_TYPE_VARBIT, -1},

    {"NUMERIC", CCI_U_TYPE_NUMERIC, -1},
    {"NUMBER", CCI_U_TYPE_NUMERIC, -1},
    {"INT", CCI_U_TYPE_INT, MAX_LEN_INTEGER},
    {"SHORT", CCI_U_TYPE_SHORT, MAX_LEN_SMALLINT},
    {"BIGINT", CCI_U_TYPE_BIGINT, MAX_LEN_BIGINT},
    {"MONETARY", CCI_U_TYPE_MONETARY, MAX_LEN_MONETARY},

    {"FLOAT", CCI_U_TYPE_FLOAT, MAX_LEN_FLOAT},
    {"DOUBLE", CCI_U_TYPE_DOUBLE, MAX_LEN_DOUBLE},

    {"DATE", CCI_U_TYPE_DATE, MAX_LEN_DATE},
    {"TIME", CCI_U_TYPE_TIME, MAX_LEN_TIME},
    {"DATETIME", CCI_U_TYPE_DATETIME, MAX_LEN_DATETIME},
    {"TIMESTAMP", CCI_U_TYPE_TIMESTAMP, MAX_LEN_TIMESTAMP},

    {"SET", CCI_U_TYPE_SET, MAX_LEN_SET},
    {"MULTISET", CCI_U_TYPE_MULTISET, MAX_LEN_MULTISET},
    {"SEQUENCE", CCI_U_TYPE_SEQUENCE, MAX_LEN_SEQUENCE},
    {"RESULTSET", CCI_U_TYPE_RESULTSET, -1},

    {"OBJECT", CCI_U_TYPE_OBJECT, MAX_LEN_OBJECT},
	{"BLOB", CCI_U_TYPE_BLOB, MAX_LEN_LOB},
	{"CLOB", CCI_U_TYPE_CLOB, MAX_LEN_LOB}
};

/************************************************************************
* PRIVATE FUNCTION PROTOTYPES
************************************************************************/

static int get_cubrid_u_type_by_name(const char *type_name);
static int get_cubrid_u_type_len(T_CCI_U_TYPE type);
static php_stream *cubrid_create_lob_stream(pdo_stmt_t *stmt, T_CCI_BLOB lob TSRMLS_DC);

/************************************************************************
* IMPLEMENTATION OF CUBRID PDO
************************************************************************/

static int cubrid_stmt_dtor(pdo_stmt_t *stmt TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt *)stmt->driver_data;

	if (S->stmt_handle) {
		if (S->bind_num > 0) {
			if (S->l_bind) {
				efree(S->l_bind);
				S->l_bind = NULL;
			}

			if (S->param_info) {
				cci_param_info_free(S->param_info);
				S->param_info = NULL;
			}

			S->bind_num = 0;
		}
		
		cci_fetch_buffer_clear(S->stmt_handle);
		cci_close_req_handle(S->stmt_handle);

		S->stmt_handle = 0;
	}
	
	efree(S);
	stmt->driver_data = NULL;
	
	return 1;
}

static int cubrid_stmt_execute(pdo_stmt_t *stmt TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt *)stmt->driver_data;

    T_CCI_COL_INFO *res_col_info;
    T_CCI_CUBRID_STMT res_sql_type;
    int res_col_count = 0;

	char exec_flag = CCI_EXEC_QUERY_ALL;
	int query_time_msec = 0;

	int cubrid_retval = 0;
	long exec_ret = 0;
    T_CCI_ERROR error;
	int i;

	if (!S->stmt_handle) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_STMT_HANDLE, NULL, NULL);
		return 0;
	}

	if ((cubrid_retval = cci_fetch_buffer_clear(S->stmt_handle)) < 0) {
		pdo_cubrid_error_stmt(stmt, cubrid_retval, NULL, NULL);
		return 0;
	}

	if (!S->l_prepare) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_NOT_PREPARED, NULL, NULL);
		return 0;	
	}

	if (S->bind_num > 0) {
		if (!S->l_bind) {
			return 0;
		}

		for (i = 0; i < S->bind_num; i++) {
			if (!S->l_bind[i]) {
				pdo_cubrid_error_stmt(stmt, CUBRID_ER_PARAM_NOT_BIND, NULL, NULL);
				return 0;
			}
		}
	}

	if (S->H->query_timeout != -1) {
		exec_flag |= CCI_EXEC_THREAD;
	}

	exec_ret = cci_execute(S->stmt_handle, exec_flag, 0, &error);
	
	if (S->H->query_timeout != -1 && exec_ret == CCI_ER_THREAD_RUNNING) {
		do {
			SLEEP_MILISEC(0, EXEC_CHECK_INTERVAL);
			query_time_msec += EXEC_CHECK_INTERVAL;

			exec_ret = cci_get_thread_result(S->H->conn_handle, &error);
		} while (exec_ret == CCI_ER_THREAD_RUNNING && query_time_msec < S->H->query_timeout * 1000);

		if (exec_ret == CCI_ER_THREAD_RUNNING) {
			pdo_cubrid_error_stmt(stmt, CUBRID_ER_EXEC_TIMEOUT, NULL, NULL);
			cci_cancel(S->H->conn_handle);
			return 0;
		}
	}

	if (exec_ret < 0) {
		if (stmt->dbh->auto_commit) {
			cci_end_tran(S->H->conn_handle, CCI_TRAN_ROLLBACK, &error);
		}

		pdo_cubrid_error_stmt(stmt, exec_ret, &error, NULL);
		return 0;
    } else {
		if (stmt->dbh->auto_commit) {
			if ((cubrid_retval = cci_end_tran(S->H->conn_handle, CCI_TRAN_COMMIT, &error)) < 0) {
				pdo_cubrid_error_stmt(stmt, cubrid_retval, &error, NULL);
				return 0;
			}
		}
	}

	res_col_info = cci_get_result_info(S->stmt_handle, &res_sql_type, &res_col_count);
    if (res_sql_type == CUBRID_STMT_SELECT && !res_col_info) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_CANNOT_GET_COLUMN_INFO, NULL, NULL);
		return 0;
    }

	S->col_info = res_col_info;
	S->sql_type = res_sql_type;
	S->col_count = res_col_count;

	stmt->column_count = res_col_count;
	stmt->row_count = exec_ret;

	switch (S->sql_type) {
    case CUBRID_STMT_SELECT:
		S->row_count = exec_ret;
		break;
    case CUBRID_STMT_INSERT:
    case CUBRID_STMT_UPDATE:
    case CUBRID_STMT_DELETE:
		S->affected_rows = exec_ret;
		break;
    case CUBRID_STMT_CALL:
		S->row_count = exec_ret;
		break;
	default:
		break;
    }

	S->cursor_pos = 1;

	for (i = 0; i < S->bind_num; i++) {
		S->l_bind[i] = 0;
	}

	return 1;
}

static int cubrid_stmt_param_hook(pdo_stmt_t *stmt, struct pdo_bound_param_data *param, 
		enum pdo_param_event event_type TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt *)stmt->driver_data;

	char *bind_value = NULL, *bind_value_type = NULL;
	int bind_value_len, bind_index;

	T_CCI_U_TYPE u_type;
	T_CCI_A_TYPE a_type;

    T_CCI_BIT *bit_value = NULL;
	T_CCI_BLOB blob = NULL;

    php_stream *stm = NULL;
	char* lobfile_name = NULL;
    char buf[CUBRID_LOB_READ_BUF_SIZE];
    pdo_uint64_t lob_start_pos = 0;
    size_t lob_read_size = 0;

	T_CCI_ERROR error;
	int cubrid_retval = 0;

	if (!S->stmt_handle) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_STMT_HANDLE, NULL, NULL);
		return 0;
	}

	if (!S->l_prepare || !param->is_param) {
		return 1;	
	}

	switch (event_type) {
	case PDO_PARAM_EVT_EXEC_PRE:
		if (param->paramno < 0 || param->paramno >= S->bind_num) {
			pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_INDEX, NULL, NULL);
			return 0;
		}

		bind_index = param->paramno + 1;

		/* driver_params: cubrid data type name (string), pass by driver_options */
		if (!param->driver_params) {
			/* if driver_params is null, use param->param_type */ 
			switch (param->param_type) {
			case PDO_PARAM_INT:
				u_type = CCI_U_TYPE_INT;

				break;
			case PDO_PARAM_STR:
#if PDO_DRIVER_API >= 20080721
			case PDO_PARAM_ZVAL:
#endif
				u_type = CCI_U_TYPE_STRING;	

				break;
			case PDO_PARAM_LOB:
				u_type = CCI_U_TYPE_BLOB;

				break;
			case PDO_PARAM_NULL:
				u_type = CCI_U_TYPE_NULL;

				break;
			case PDO_PARAM_STMT:
			default:
				pdo_cubrid_error_stmt(stmt, CUBRID_ER_NOT_SUPPORTED_TYPE, NULL, NULL);
				return 0;
			}
		} else {
			convert_to_string(param->driver_params);
			bind_value_type = Z_STRVAL_P(param->driver_params);

			u_type = get_cubrid_u_type_by_name(bind_value_type);

			if (u_type == CCI_U_TYPE_UNKNOWN || u_type == CCI_U_TYPE_SET || 
				u_type == CCI_U_TYPE_MULTISET || u_type == CCI_U_TYPE_SEQUENCE) {
				pdo_cubrid_error_stmt(stmt, CUBRID_ER_NOT_SUPPORTED_TYPE, NULL, NULL);
				return 0;
			}
		}

		if (u_type == CCI_U_TYPE_NULL || Z_TYPE_P(param->parameter) == IS_NULL) {
			cubrid_retval = cci_bind_param(S->stmt_handle, bind_index, CCI_A_TYPE_STR, NULL, u_type, 0);
		} else {
			if (u_type == CCI_U_TYPE_BLOB || u_type == CCI_U_TYPE_CLOB) {
				if (Z_TYPE_P(param->parameter) == IS_RESOURCE) {
					php_stream_from_zval_no_verify(stm, &param->parameter);
					if (stm) {
						SEPARATE_ZVAL_IF_NOT_REF(&param->parameter);
						Z_TYPE_P(param->parameter) = IS_STRING;
						Z_STRLEN_P(param->parameter) = php_stream_copy_to_mem(stm, &Z_STRVAL_P(param->parameter), PHP_STREAM_COPY_ALL, 0);
					} else {
						php_error_docref(NULL TSRMLS_CC, E_WARNING, "Expected a stream resource when param type is LOB\n");
						return 0;
					}
				} else {
					/* file name */
					convert_to_string(param->parameter);
					lobfile_name = Z_STRVAL_P(param->parameter);

					if (!(stm = php_stream_open_wrapper(lobfile_name, "r", REPORT_ERRORS, NULL))) {
						return 0;
					}
				}
			} else {
				convert_to_string(param->parameter);
			}

			bind_value = Z_STRVAL_P(param->parameter);
			bind_value_len = Z_STRLEN_P(param->parameter);

			switch (u_type) {
			case CCI_U_TYPE_BLOB:
					a_type = CCI_A_TYPE_BLOB;
					break;
			case CCI_U_TYPE_CLOB:
					a_type = CCI_A_TYPE_CLOB;
					break;
			case CCI_U_TYPE_BIT:
			case CCI_U_TYPE_VARBIT:
					a_type = CCI_A_TYPE_BIT;
					break;
			default:
					a_type = CCI_A_TYPE_STR;
					break;
			}

			if (u_type == CCI_U_TYPE_BLOB || u_type == CCI_U_TYPE_CLOB) {
				if ((cubrid_retval = cci_blob_new(S->H->conn_handle, &blob, &error)) < 0) {
					pdo_cubrid_error_stmt(stmt, cubrid_retval, &error, NULL);

					if (lobfile_name) {
						php_stream_close(stm);
					}

					return 0;
				}

				if (lobfile_name) {
					while (!php_stream_eof(stm)) { 
						lob_read_size = php_stream_read(stm, buf, CUBRID_LOB_READ_BUF_SIZE); 

						if ((cubrid_retval = cci_blob_write(S->H->conn_handle, blob, 
										lob_start_pos, lob_read_size, buf, &error)) < 0) {
							pdo_cubrid_error_stmt(stmt, cubrid_retval, &error, NULL);
							php_stream_close(stm);

							return 0;
						}        

						lob_start_pos += lob_read_size;
					}
					
					php_stream_close(stm);
				} else {
					if ((cubrid_retval = cci_blob_write(S->H->conn_handle, blob, 0, bind_value_len, bind_value, &error)) < 0) {
						pdo_cubrid_error_stmt(stmt, cubrid_retval, &error, NULL);
						return 0;
					}
				}

				cubrid_retval = cci_bind_param(S->stmt_handle, bind_index, a_type, (void *) blob, u_type, CCI_BIND_PTR); 

				S->blob = blob;

				efree(Z_STRVAL_P(param->parameter));
				FREE_ZVAL(param->parameter);
			} else if (u_type == CCI_U_TYPE_BIT) {
				bit_value = (T_CCI_BIT *) emalloc(sizeof(T_CCI_BIT));
				bit_value->size = bind_value_len;
				bit_value->buf = bind_value;

				cubrid_retval = cci_bind_param(S->stmt_handle, bind_index, a_type, (void *) bit_value, u_type, 0);

				efree(bit_value);
			} else {
				cubrid_retval = cci_bind_param(S->stmt_handle, bind_index, a_type, bind_value, u_type, 0);
			} 
		}

		if (cubrid_retval != 0 || !S->l_bind) {
			pdo_cubrid_error_stmt(stmt, cubrid_retval, NULL, NULL);
			return 0;
		}

		S->l_bind[param->paramno] = 1;
		
		break;
	case PDO_PARAM_EVT_EXEC_POST:
		if (S->blob) {
			cci_blob_free(S->blob);
			S->blob = NULL;
		}

		break;
	case PDO_PARAM_EVT_ALLOC:
	case PDO_PARAM_EVT_FREE:
	case PDO_PARAM_EVT_FETCH_PRE:
	case PDO_PARAM_EVT_FETCH_POST:
	case PDO_PARAM_EVT_NORMALIZE:
		break;
	default:
		break;
	}

	return 1;
}

static int cubrid_stmt_fetch(pdo_stmt_t *stmt, enum pdo_fetch_orientation ori, long offset TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt *)stmt->driver_data;
	T_CCI_CURSOR_POS origin;

	long abs_offset = 0;
	int cubrid_retval = 0;
    T_CCI_ERROR error;

	if (!S->stmt_handle) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_STMT_HANDLE, NULL, NULL);
		return 0;
	}

	switch (ori) {
	case PDO_FETCH_ORI_FIRST:
		origin = CCI_CURSOR_FIRST;	
		offset = 1;

		abs_offset = 1;
		break;
	case PDO_FETCH_ORI_LAST:
		origin = CCI_CURSOR_LAST;	
		offset = 1;

		if (S->cursor_type == PDO_CURSOR_FWDONLY) {
			abs_offset = S->row_count;
		} else {
			/* -1 mean the last */
			abs_offset = -1;
		}
		break;
	case PDO_FETCH_ORI_NEXT:
		origin = CCI_CURSOR_CURRENT;	
		offset = 1;

		abs_offset = S->cursor_pos + 1;
		break;
	case PDO_FETCH_ORI_PRIOR:
		origin = CCI_CURSOR_CURRENT;
		offset = -1;

		abs_offset = S->cursor_pos - 1;
		break;
	case PDO_FETCH_ORI_ABS:
		origin = CCI_CURSOR_FIRST;

		abs_offset = offset;
		break;
	case PDO_FETCH_ORI_REL:
		origin = CCI_CURSOR_CURRENT;

		abs_offset = S->cursor_pos + offset;
		break;
	default:
		return 0;
	}

	if (S->cursor_type == PDO_CURSOR_FWDONLY && abs_offset != -1 && abs_offset < S->cursor_pos) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_CURSOR_POS, NULL, NULL);
		return 0;
	} 

	cubrid_retval = cci_cursor(S->stmt_handle, offset, origin, &error);
    if (cubrid_retval == CCI_ER_NO_MORE_DATA) {
		return 0;
    }

	if (cubrid_retval < 0) {
		pdo_cubrid_error_stmt(stmt, cubrid_retval, &error, NULL);
		return 0;
	}

	S->cursor_pos = abs_offset;

	cubrid_retval = cci_fetch(S->stmt_handle, &error);
	if (cubrid_retval < 0) {
		pdo_cubrid_error_stmt(stmt, cubrid_retval, &error, NULL);
		return 0;
	}

	return 1;
}

static int cubrid_stmt_describe_col(pdo_stmt_t *stmt, int colno TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt *)stmt->driver_data;
	struct pdo_column_data *cols = stmt->columns;

    T_CCI_U_TYPE type;
	long type_maxlen;

	if (!S->col_info) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_CANNOT_GET_COLUMN_INFO, NULL, NULL);
		return 0;
	}
	
	if (S->sql_type != CUBRID_STMT_SELECT) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_SQL_TYPE, NULL, NULL);
		return 0;
	}

	if (colno < 0 || colno > S->col_count - 1) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_INDEX, NULL, NULL);
		return 0;
	}

	cols[colno].name = estrdup(CCI_GET_RESULT_INFO_NAME (S->col_info, colno + 1));
	cols[colno].namelen = strlen(cols[colno].name);
	cols[colno].precision = CCI_GET_RESULT_INFO_PRECISION (S->col_info, colno + 1);

	type = CCI_GET_COLLECTION_DOMAIN(CCI_GET_RESULT_INFO_TYPE(S->col_info, colno + 1));
	if ((type_maxlen = get_cubrid_u_type_len(type)) == -1) {
		type_maxlen = CCI_GET_RESULT_INFO_PRECISION(S->col_info, colno + 1); 
		if (type == CCI_U_TYPE_NUMERIC) {
			type_maxlen += 2; /* "," + "-" */
		}
    }

	if (CCI_IS_COLLECTION_TYPE(CCI_GET_RESULT_INFO_TYPE(S->col_info, colno + 1))) {
		type_maxlen = MAX_LEN_SET;
    }

	cols[colno].maxlen = type_maxlen;

	if (type == CCI_U_TYPE_BLOB || type == CCI_U_TYPE_CLOB) {
		cols[colno].param_type = PDO_PARAM_LOB;
	} else {
		/* should make decision later, use STR now */
		cols[colno].param_type = PDO_PARAM_STR;
	}

	return 1;
}

static int cubrid_stmt_get_col_data(pdo_stmt_t *stmt, int colno, char **ptr, unsigned long *len, 
		int *caller_frees TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt *)stmt->driver_data;
	struct pdo_column_data *cols = stmt->columns; 

	T_CCI_U_TYPE u_type;
	T_CCI_A_TYPE a_type;
	T_CCI_BLOB blob = NULL;

	int cubrid_retval = 0;
	char *res_buf = NULL;
    int ind = 0;

	if (!S->stmt_handle) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_STMT_HANDLE, NULL, NULL);
		return 0;
	}

	if (colno < 0 || colno >= S->col_count) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_INVALID_INDEX, NULL, NULL);
		return 0;
	}

	*caller_frees = 0;

	switch (cols[colno].param_type) {
	case PDO_PARAM_STR:
#if PDO_DRIVER_API >= 20080721
	case PDO_PARAM_ZVAL:
#endif
		if ((cubrid_retval = cci_get_data(S->stmt_handle, colno + 1, CCI_A_TYPE_STR, &res_buf, &ind)) < 0) {
			pdo_cubrid_error_stmt(stmt, cubrid_retval, NULL, NULL);
			return 0;
		}

		if (ind < 0) {
			*ptr = NULL;
			*len = 0;
		} else {
			*ptr = res_buf;
			*len = ind;
		}

		break;
	case PDO_PARAM_LOB:
		u_type = CCI_GET_RESULT_INFO_TYPE(S->col_info, colno + 1);

		if (u_type == CCI_U_TYPE_BLOB) {
			a_type = CCI_A_TYPE_BLOB;
		} else if (u_type == CCI_U_TYPE_CLOB) {
			a_type = CCI_A_TYPE_CLOB;
		} else {
			return 0;
		}

		if ((cubrid_retval = cci_get_data(S->stmt_handle, colno + 1, a_type, (void *) &blob, &ind)) < 0) {
			pdo_cubrid_error_stmt(stmt, cubrid_retval, NULL, NULL);
			return 0;
		}

		if (ind < 0) {
			return 0;
		}

		*ptr = (char *)cubrid_create_lob_stream(stmt, blob TSRMLS_CC);
		*len = 0;

		return *ptr ? 1 : 0;
	case PDO_PARAM_INT:

		break;
	default:
		return 0;
	}
	
	return 1;
}

static int cubrid_stmt_next_rowset(pdo_stmt_t *stmt TSRMLS_DC)
{
	pdo_cubrid_stmt *S = (pdo_cubrid_stmt*)stmt->driver_data;

    T_CCI_COL_INFO *res_col_info;
    T_CCI_CUBRID_STMT res_sql_type;
    int res_col_count = 0;

	int exec_ret = 0;
	T_CCI_ERROR error;

	cci_fetch_buffer_clear(S->stmt_handle);
	cci_close_req_handle(S->stmt_handle);

	exec_ret = cci_next_result(S->stmt_handle, &error);
	if (exec_ret == CAS_ER_NO_MORE_RESULT_SET) {
		return 0;
	}

	if (exec_ret < 0) {
		pdo_cubrid_error_stmt(stmt, exec_ret, &error, NULL);
		return 0;
	}

	res_col_info = cci_get_result_info(S->stmt_handle, &res_sql_type, &res_col_count);
    if (res_sql_type == CUBRID_STMT_SELECT && !res_col_info) {
		pdo_cubrid_error_stmt(stmt, CUBRID_ER_CANNOT_GET_COLUMN_INFO, NULL, NULL);
		return 0;
    }

	S->col_info = res_col_info;
	S->sql_type = res_sql_type;
	S->col_count = res_col_count;

	stmt->column_count = res_col_count;
	stmt->row_count = exec_ret;

	switch (S->sql_type) {
    case CUBRID_STMT_SELECT:
		S->row_count = exec_ret;
		break;
    case CUBRID_STMT_INSERT:
    case CUBRID_STMT_UPDATE:
    case CUBRID_STMT_DELETE:
		S->affected_rows = exec_ret;
		break;
    case CUBRID_STMT_CALL:
		S->row_count = exec_ret;
		break;
	default:
		break;
    }

	S->cursor_pos = 1;

	return 1;
}

struct pdo_stmt_methods cubrid_stmt_methods = {
    cubrid_stmt_dtor, 
	cubrid_stmt_execute, 
	cubrid_stmt_fetch, 
	cubrid_stmt_describe_col, 
	cubrid_stmt_get_col_data, 
	cubrid_stmt_param_hook, 
	NULL, /* set attr */
	NULL, /* get attr */
	NULL, /* get column meta */
	cubrid_stmt_next_rowset,
	NULL, /* cursor closer */
};

/************************************************************************
* PRIVATE FUNCTIONS IMPLEMENTATION
************************************************************************/

static int get_cubrid_u_type_by_name(const char *type_name)
{
    int i;
    int size = sizeof(db_type_info)/sizeof(db_type_info[0]);

    for (i = 0; i < size; i++) {
		if (strcasecmp(type_name, db_type_info[i].type_name) == 0) {
			return db_type_info[i].cubrid_u_type;
		}
    }

    return CCI_U_TYPE_UNKNOWN;
}

static int get_cubrid_u_type_len(T_CCI_U_TYPE type)
{
    int i;
    int size = sizeof(db_type_info)/sizeof(db_type_info[0]);
    DB_TYPE_INFO type_info;

    for (i = 0; i < size; i++) {
		type_info = db_type_info[i];
		if (type == type_info.cubrid_u_type) {
			return type_info.len;
		}
    }

    return 0;
}

struct cubrid_lob_self {
	pdo_stmt_t *stmt;
	pdo_cubrid_stmt *S;
	T_CCI_BLOB lob;
	pdo_uint64_t offset;
	pdo_uint64_t size;
};

static size_t cubrid_blob_write(php_stream *stream, const char *buf, size_t count TSRMLS_DC)
{
	return 0;
}

static size_t cubrid_blob_read(php_stream *stream, char *buf, size_t count TSRMLS_DC)
{
	struct cubrid_lob_self *self = (struct cubrid_lob_self*)stream->abstract;

	T_CCI_ERROR error;
	int read_len;
	int cubrid_retval = 0;

	if ((self->offset + count) < self->size) {
		read_len = count;	
	} else {
		read_len = self->size - self->offset;	
		stream->eof = 1;
	}

	if (read_len) {
		cubrid_retval = cci_blob_read(self->S->H->conn_handle, self->lob, self->offset, read_len, buf, &error);
		if (cubrid_retval < 0) {
			return 0;
		} else {
			self->offset += cubrid_retval;
		}
	}

	return (size_t)cubrid_retval;
}

static int cubrid_blob_close(php_stream *stream, int close_handle TSRMLS_DC)
{
	struct cubrid_lob_self *self = (struct cubrid_lob_self*)stream->abstract;
	pdo_stmt_t *stmt = self->stmt;

	if (close_handle) {
		cci_blob_free(self->lob);
	}
	efree(self);

	php_pdo_stmt_delref(stmt TSRMLS_CC);

	return 0;
}

static int cubrid_blob_flush(php_stream *stream TSRMLS_DC)
{
	return 0;
}

static php_stream_ops cubrid_blob_stream_ops = {
	cubrid_blob_write,
	cubrid_blob_read,
	cubrid_blob_close,
	cubrid_blob_flush,
	"pdo_cubrid blob stream",
	NULL, /* seek */
	NULL, /* cast */
	NULL, /* stat */
	NULL /* set_option */
};

static php_stream *cubrid_create_lob_stream(pdo_stmt_t *stmt, T_CCI_BLOB lob TSRMLS_DC)
{
	php_stream *stm;
	struct cubrid_lob_self *self = ecalloc(1, sizeof(struct cubrid_lob_self));

	self->lob = lob;
	self->offset = 0;
	self->size = cci_blob_size(lob);
	self->stmt = stmt;
	self->S = (pdo_cubrid_stmt *)stmt->driver_data;
	
	stm = php_stream_alloc(&cubrid_blob_stream_ops, self, 0, "rb");
	if (stm) {
		php_pdo_stmt_addref(stmt TSRMLS_CC);
		return stm;
	}

	efree(self);
	return NULL;
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=manual
 * vim<600: noet sw=4 ts=4
 */
