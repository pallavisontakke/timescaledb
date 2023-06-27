/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <fmgr.h>
#include <funcapi.h>
#include <access/htup_details.h>
#include <foreign/foreign.h>
#include <utils/builtins.h>

#include "data_node.h"
#include "export.h"

TS_FUNCTION_INFO_V1(ts_test_data_node_show);
TS_FUNCTION_INFO_V1(ts_unchecked_add_data_node);
TS_FUNCTION_INFO_V1(ts_data_node_exec);

/*
 * Tests the ts_data_node_get_node_name_list() function.
 */
Datum
ts_test_data_node_show(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;

	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;
		TupleDesc tupdesc;
		List *node_names;

		funcctx = SRF_FIRSTCALL_INIT();
		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("function returning record called in context "
							"that cannot accept type record")));

		node_names = data_node_get_node_name_list();
		funcctx->user_fctx = node_names;
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);
		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();

	if (list_length((List *) funcctx->user_fctx) > 0)
	{
		Datum values[3];
		bool nulls[3] = { true };
		HeapTuple tuple;
		List *node_names = funcctx->user_fctx;
		const char *node_name = linitial(node_names);
		ForeignServer *server = GetForeignServerByName(node_name, false);
		ListCell *lc;

		funcctx->user_fctx = list_delete_first(node_names);
		funcctx->call_cntr++;

		NameData dbnamedata;
		NameData node_namedata;
		namestrcpy(&node_namedata, node_name);
		values[0] = NameGetDatum(&node_namedata);
		nulls[0] = false;

		foreach (lc, server->options)
		{
			DefElem *elem = lfirst(lc);
			Assert(IsA(elem->arg, String));

			if (strcmp("host", elem->defname) == 0)
			{
				values[1] = CStringGetTextDatum(defGetString(elem));
				nulls[1] = false;
			}
			else if (strcmp("dbname", elem->defname) == 0)
			{
				namestrcpy(&dbnamedata, defGetString(elem));
				values[2] = NameGetDatum(&dbnamedata);
				nulls[2] = false;
			}
		}

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Performs a data node add without setting distributed id or enforcing topolgy constraints.
 */
Datum
ts_unchecked_add_data_node(PG_FUNCTION_ARGS)
{
	return data_node_add_without_dist_id(fcinfo);
}

/*
 * Execute a command on a data node.
 *
 * Mostly for debugging connection execution functions.
 */
Datum
ts_data_node_exec(PG_FUNCTION_ARGS)
{
	const char *nodename = PG_GETARG_CSTRING(0);
	TSConnection *conn = data_node_get_connection(nodename, REMOTE_TXN_NO_PREP_STMT, true);
	const text *cmdstr;
	PGresult *res;

	if (PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("command string cannot be NULL")));

	cmdstr = PG_GETARG_TEXT_P(1);
	res = remote_connection_exec(conn, text_to_cstring(cmdstr));

	if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		PQclear(res);
		remote_connection_elog(conn, ERROR);
	}

	PG_RETURN_VOID();
}
