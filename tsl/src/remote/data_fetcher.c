/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>

#include "data_fetcher.h"
#include "cursor_fetcher.h"
#include "copy_fetcher.h"
#include "guc.h"
#include "errors.h"

#define DEFAULT_FETCH_SIZE 100

void
data_fetcher_init(DataFetcher *df, TSConnection *conn, const char *stmt, StmtParams *params,
				  TupleFactory *tf)
{
	Assert(df != NULL);
	Assert(stmt != NULL);

	memset(df, 0, sizeof(DataFetcher));
	df->tuples = NULL;
	df->conn = conn;
	df->stmt = pstrdup(stmt);
	df->stmt_params = params;
	df->tf = tf;

	tuplefactory_set_per_tuple_mctx_reset(df->tf, false);
	df->batch_mctx = AllocSetContextCreate(CurrentMemoryContext,
										   "data fetcher tuple batch data",
										   ALLOCSET_DEFAULT_SIZES);
	df->tuple_mctx = df->batch_mctx;
	df->req_mctx = AllocSetContextCreate(CurrentMemoryContext,
										 "data fetcher async request/response",
										 ALLOCSET_DEFAULT_SIZES);
	df->fetch_size = DEFAULT_FETCH_SIZE;
}

void
data_fetcher_validate(DataFetcher *df)
{
	/* ANALYZE command is accessing random tuples so we should never fail here when running ANALYZE
	 */
	if (df->next_tuple_idx != 0 && df->next_tuple_idx < df->num_tuples)
		ereport(ERROR,
				(errcode(ERRCODE_TS_INTERNAL_ERROR),
				 errmsg("invalid data fetcher state. sql: %s", df->stmt),
				 errhint("Shouldn't fetch new data before consuming existing.")));
}

void
data_fetcher_store_tuple(DataFetcher *df, int row, TupleTableSlot *slot)
{
	if (row >= df->num_tuples)
	{
		/* No point in another fetch if we already detected EOF, though. */
		if (df->eof || df->funcs->fetch_data(df) == 0)
		{
			ExecClearTuple(slot);
			return;
		}

		/* More data was fetched so need to reset row index */
		row = 0;
		Assert(row == df->next_tuple_idx);
	}

	Assert(df->tuples != NULL);
	Assert(row >= 0 && row < df->num_tuples);

	/*
	 * Return the next tuple. Must force the tuple into the slot since
	 * CustomScan initializes ss_ScanTupleSlot to a VirtualTupleTableSlot
	 * while we're storing a HeapTuple.
	 */
	ExecForceStoreHeapTuple(df->tuples[row], slot, /* shouldFree = */ false);
}

void
data_fetcher_store_next_tuple(DataFetcher *df, TupleTableSlot *slot)
{
	data_fetcher_store_tuple(df, df->next_tuple_idx, slot);

	if (!TupIsNull(slot))
		df->next_tuple_idx++;

	Assert(df->next_tuple_idx <= df->num_tuples);
}

void
data_fetcher_set_fetch_size(DataFetcher *df, int fetch_size)
{
	df->fetch_size = fetch_size;
}

void
data_fetcher_set_tuple_mctx(DataFetcher *df, MemoryContext mctx)
{
	Assert(mctx != NULL);
	df->tuple_mctx = mctx;
}

void
data_fetcher_reset(DataFetcher *df)
{
	df->tuples = NULL;
	df->num_tuples = 0;
	df->next_tuple_idx = 0;
	df->batch_count = 0;
	df->eof = false;
	MemoryContextReset(df->req_mctx);
	MemoryContextReset(df->batch_mctx);
}

/*
 * This is the default implementation of starting the scan with the new
 * parameters. It just closes the current scan and updates the parameter
 * values, and the next scan is initialized from scratch. The prepared statement
 * fetcher is more efficient than that, and reuses the prepared statement.
 */
void
data_fetcher_rescan(DataFetcher *df, StmtParams *params)
{
	df->funcs->close(df);
	df->stmt_params = params;
}

void
data_fetcher_free(DataFetcher *df)
{
	df->funcs->close(df);
	pfree(df);
}
