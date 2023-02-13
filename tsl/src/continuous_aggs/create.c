/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

/*
 * This file contains the code for processing continuous aggregate
 * DDL statements which are of the form:
 *
 * CREATE MATERIALIZED VIEW <name> WITH (ts_continuous = [option] )
 * AS  <select query>
 * The entry point for the code is
 * tsl_process_continuous_agg_viewstmt
 * The bulk of the code that creates the underlying tables/views etc. is in
 * cagg_create.
 */
#include <postgres.h>
#include <access/reloptions.h>
#include <access/sysattr.h>
#include <access/xact.h>
#include <catalog/index.h>
#include <catalog/indexing.h>
#include <catalog/pg_aggregate.h>
#include <catalog/pg_collation.h>
#include <catalog/pg_namespace.h>
#include <catalog/pg_trigger.h>
#include <catalog/pg_type.h>
#include <catalog/toasting.h>
#include <commands/defrem.h>
#include <commands/tablecmds.h>
#include <commands/tablespace.h>
#include <commands/trigger.h>
#include <commands/view.h>
#include <miscadmin.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/nodes.h>
#include <nodes/parsenodes.h>
#include <nodes/pg_list.h>
#include <optimizer/clauses.h>
#include <optimizer/optimizer.h>
#include <optimizer/tlist.h>
#include <parser/analyze.h>
#include <parser/parse_func.h>
#include <parser/parse_oper.h>
#include <parser/parse_relation.h>
#include <parser/parse_type.h>
#include <rewrite/rewriteHandler.h>
#include <rewrite/rewriteManip.h>
#include <utils/acl.h>
#include <utils/rel.h>
#include <utils/builtins.h>
#include <utils/catcache.h>
#include <utils/regproc.h>
#include <utils/ruleutils.h>
#include <utils/syscache.h>
#include <utils/typcache.h>
#include <optimizer/prep.h>

#include "create.h"

#include "ts_catalog/catalog.h"
#include "ts_catalog/continuous_agg.h"
#include "dimension.h"
#include "extension_constants.h"
#include "func_cache.h"
#include "hypertable_cache.h"
#include "hypertable.h"
#include "invalidation.h"
#include "dimension.h"
#include "ts_catalog/continuous_agg.h"
#include "options.h"
#include "time_utils.h"
#include "utils.h"
#include "errors.h"
#include "refresh.h"
#include "remote/dist_commands.h"
#include "ts_catalog/hypertable_data_node.h"
#include "deparse.h"
#include "timezones.h"

#define FINALFN "finalize_agg"
#define PARTIALFN "partialize_agg"
#define CHUNKIDFROMRELID "chunk_id_from_relid"
#define DEFAULT_MATPARTCOLUMN_NAME "time_partition_col"
#define MATPARTCOL_INTERVAL_FACTOR 10
#define HT_DEFAULT_CHUNKFN "calculate_chunk_interval"
#define CAGG_INVALIDATION_TRIGGER "continuous_agg_invalidation_trigger"
#define BOUNDARY_FUNCTION "cagg_watermark"
#define INTERNAL_TO_DATE_FUNCTION "to_date"
#define INTERNAL_TO_TSTZ_FUNCTION "to_timestamp"
#define INTERNAL_TO_TS_FUNCTION "to_timestamp_without_timezone"
#define CONTINUOUS_AGG_MAX_JOIN_RELATIONS 2

#define PRINT_MATCOLNAME(colbuf, type, original_query_resno, colno)                                \
	do                                                                                             \
	{                                                                                              \
		int ret = snprintf(colbuf, NAMEDATALEN, "%s_%d_%d", type, original_query_resno, colno);    \
		if (ret < 0 || ret >= NAMEDATALEN)                                                         \
			ereport(ERROR,                                                                         \
					(errcode(ERRCODE_INTERNAL_ERROR),                                              \
					 errmsg("bad materialization table column name")));                            \
	} while (0);

#define PRINT_MATINTERNAL_NAME(buf, prefix, hypertable_id)                                         \
	do                                                                                             \
	{                                                                                              \
		int ret = snprintf(buf, NAMEDATALEN, prefix, hypertable_id);                               \
		if (ret < 0 || ret > NAMEDATALEN)                                                          \
		{                                                                                          \
			ereport(ERROR,                                                                         \
					(errcode(ERRCODE_INTERNAL_ERROR),                                              \
					 errmsg("bad materialization internal name")));                                \
		}                                                                                          \
	} while (0);

/* Note that we set rowsecurity to false here. */
#define CAGG_MAKEQUERY(selquery, srcquery)                                                         \
	do                                                                                             \
	{                                                                                              \
		(selquery) = makeNode(Query);                                                              \
		(selquery)->commandType = CMD_SELECT;                                                      \
		(selquery)->querySource = (srcquery)->querySource;                                         \
		(selquery)->queryId = (srcquery)->queryId;                                                 \
		(selquery)->canSetTag = (srcquery)->canSetTag;                                             \
		(selquery)->utilityStmt = copyObject((srcquery)->utilityStmt);                             \
		(selquery)->resultRelation = 0;                                                            \
		(selquery)->hasAggs = true;                                                                \
		(selquery)->hasRowSecurity = false;                                                        \
		(selquery)->rtable = NULL;                                                                 \
	} while (0);

typedef struct MatTableColumnInfo
{
	List *matcollist;		 /* column defns for materialization tbl*/
	List *partial_seltlist;	 /* tlist entries for populating the materialization table columns */
	List *partial_grouplist; /* group clauses used for populating the materialization table */
	List *mat_groupcolname_list; /* names of columns that are populated by the group-by clause
									correspond to the partial_grouplist.
									time_bucket column is not included here: it is the
									matpartcolname */
	int matpartcolno;			 /*index of partitioning column in matcollist */
	char *matpartcolname;		 /*name of the partition column */
} MatTableColumnInfo;

typedef struct FinalizeQueryInfo
{
	List *final_seltlist;	/* select target list for finalize query */
	Node *final_havingqual; /* having qual for finalize query */
	Query *final_userquery; /* user query used to compute the finalize_query */
	bool finalized;			/* finalized form? */
} FinalizeQueryInfo;

typedef struct CAggTimebucketInfo
{
	int32 htid;						/* hypertable id */
	int32 parent_mat_hypertable_id; /* parent materialization hypertable id */
	Oid htoid;						/* hypertable oid */
	AttrNumber htpartcolno;			/* primary partitioning column of raw hypertable */
									/* This should also be the column used by time_bucket */
	Oid htpartcoltype;
	int64 htpartcol_interval_len; /* interval length setting for primary partitioning column */
	int64 bucket_width;			  /* bucket_width of time_bucket, stores BUCKET_WIDTH_VARIABLE for
									 variable-sized buckets */
	Oid bucket_width_type;		  /* type of bucket_width */
	Interval *interval;			  /* stores the interval, NULL if not specified */
	const char *timezone;		  /* the name of the timezone, NULL if not specified */

	FuncExpr *bucket_func; /* function call expr of the bucketing function */
	/*
	 * Custom origin value stored as UTC timestamp.
	 * If not specified, stores infinity.
	 */
	Timestamp origin;
} CAggTimebucketInfo;

typedef struct AggPartCxt
{
	struct MatTableColumnInfo *mattblinfo;
	bool added_aggref_col;
	/*
	 * Set to true when you come across a Var
	 * that is not inside an Aggref node.
	 */
	bool var_outside_of_aggref;
	Oid ignore_aggoid;
	int original_query_resno;
	/*
	 * "Original variables" are the Var nodes of the target list of the original
	 * CREATE MATERIALIZED VIEW query. "Mapped variables" are the Var nodes of the materialization
	 * table columns. The partialization query is the one that populates those columns. The
	 * finalization query should use the "mapped variables" to populate the user view.
	 */
	List *orig_vars; /* List of Var nodes that have been mapped to materialization table columns */
	List *mapped_vars; /* List of Var nodes of the corresponding materialization table columns */
					   /* orig_vars and mapped_vars lists are mapped 1 to 1 */
} AggPartCxt;

/* STATIC functions defined on the structs above. */
static void mattablecolumninfo_init(MatTableColumnInfo *matcolinfo, List *grouplist);
static Var *mattablecolumninfo_addentry(MatTableColumnInfo *out, Node *input,
										int original_query_resno, bool finalized,
										bool *skip_adding);
static void mattablecolumninfo_addinternal(MatTableColumnInfo *matcolinfo);
static int32 mattablecolumninfo_create_materialization_table(
	MatTableColumnInfo *matcolinfo, int32 hypertable_id, RangeVar *mat_rel,
	CAggTimebucketInfo *origquery_tblinfo, bool create_addl_index, char *tablespacename,
	char *table_access_method, ObjectAddress *mataddress);
static Query *mattablecolumninfo_get_partial_select_query(MatTableColumnInfo *mattblinfo,
														  Query *userview_query, bool finalized);

static void caggtimebucketinfo_init(CAggTimebucketInfo *src, int32 hypertable_id,
									Oid hypertable_oid, AttrNumber hypertable_partition_colno,
									Oid hypertable_partition_coltype,
									int64 hypertable_partition_col_interval,
									int32 parent_mat_hypertable_id);
static void caggtimebucket_validate(CAggTimebucketInfo *tbinfo, List *groupClause,
									List *targetList);
static void finalizequery_init(FinalizeQueryInfo *inp, Query *orig_query,
							   MatTableColumnInfo *mattblinfo);
static Query *finalizequery_get_select_query(FinalizeQueryInfo *inp, List *matcollist,
											 ObjectAddress *mattbladdress, char *relname);
static bool function_allowed_in_cagg_definition(Oid funcid);

static Const *cagg_boundary_make_lower_bound(Oid type);
static Oid cagg_get_boundary_converter_funcoid(Oid typoid);
static Oid relation_oid(NameData schema, NameData name);
static Query *build_union_query(CAggTimebucketInfo *tbinfo, int matpartcolno, Query *q1, Query *q2,
								int materialize_htid);
static Query *destroy_union_query(Query *q);

/*
 * Create a entry for the materialization table in table CONTINUOUS_AGGS.
 */
static void
create_cagg_catalog_entry(int32 matht_id, int32 rawht_id, const char *user_schema,
						  const char *user_view, const char *partial_schema,
						  const char *partial_view, int64 bucket_width, bool materialized_only,
						  const char *direct_schema, const char *direct_view, const bool finalized,
						  const int32 parent_mat_hypertable_id)
{
	Catalog *catalog = ts_catalog_get();
	Relation rel;
	TupleDesc desc;
	NameData user_schnm, user_viewnm, partial_schnm, partial_viewnm, direct_schnm, direct_viewnm;
	Datum values[Natts_continuous_agg];
	bool nulls[Natts_continuous_agg] = { false };
	CatalogSecurityContext sec_ctx;

	namestrcpy(&user_schnm, user_schema);
	namestrcpy(&user_viewnm, user_view);
	namestrcpy(&partial_schnm, partial_schema);
	namestrcpy(&partial_viewnm, partial_view);
	namestrcpy(&direct_schnm, direct_schema);
	namestrcpy(&direct_viewnm, direct_view);
	rel = table_open(catalog_get_table_id(catalog, CONTINUOUS_AGG), RowExclusiveLock);
	desc = RelationGetDescr(rel);

	memset(values, 0, sizeof(values));
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_mat_hypertable_id)] = matht_id;
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_raw_hypertable_id)] = rawht_id;

	if (parent_mat_hypertable_id == INVALID_HYPERTABLE_ID)
		nulls[AttrNumberGetAttrOffset(Anum_continuous_agg_parent_mat_hypertable_id)] = true;
	else
	{
		values[AttrNumberGetAttrOffset(Anum_continuous_agg_parent_mat_hypertable_id)] =
			parent_mat_hypertable_id;
	}

	values[AttrNumberGetAttrOffset(Anum_continuous_agg_user_view_schema)] =
		NameGetDatum(&user_schnm);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_user_view_name)] =
		NameGetDatum(&user_viewnm);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_partial_view_schema)] =
		NameGetDatum(&partial_schnm);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_partial_view_name)] =
		NameGetDatum(&partial_viewnm);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_bucket_width)] = Int64GetDatum(bucket_width);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_direct_view_schema)] =
		NameGetDatum(&direct_schnm);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_direct_view_name)] =
		NameGetDatum(&direct_viewnm);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_materialize_only)] =
		BoolGetDatum(materialized_only);
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_finalized)] = BoolGetDatum(finalized);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_insert_values(rel, desc, values, nulls);
	ts_catalog_restore_user(&sec_ctx);
	table_close(rel, RowExclusiveLock);
}

/*
 * Create a entry for the materialization table in table
 * CONTINUOUS_AGGS_BUCKET_FUNCTION.
 */
static void
create_bucket_function_catalog_entry(int32 matht_id, bool experimental, const char *name,
									 const char *bucket_width, const char *origin,
									 const char *timezone)
{
	Catalog *catalog = ts_catalog_get();
	Relation rel;
	TupleDesc desc;
	Datum values[Natts_continuous_aggs_bucket_function];
	bool nulls[Natts_continuous_aggs_bucket_function] = { false };
	CatalogSecurityContext sec_ctx;

	rel = table_open(catalog_get_table_id(catalog, CONTINUOUS_AGGS_BUCKET_FUNCTION),
					 RowExclusiveLock);
	desc = RelationGetDescr(rel);

	memset(values, 0, sizeof(values));
	values[AttrNumberGetAttrOffset(Anum_continuous_agg_mat_hypertable_id)] = matht_id;
	values[AttrNumberGetAttrOffset(Anum_continuous_aggs_bucket_function_experimental)] =
		BoolGetDatum(experimental);
	values[AttrNumberGetAttrOffset(Anum_continuous_aggs_bucket_function_name)] =
		CStringGetTextDatum(name);
	values[AttrNumberGetAttrOffset(Anum_continuous_aggs_bucket_function_bucket_width)] =
		CStringGetTextDatum(bucket_width);
	values[AttrNumberGetAttrOffset(Anum_continuous_aggs_bucket_function_origin)] =
		CStringGetTextDatum(origin);
	values[AttrNumberGetAttrOffset(Anum_continuous_aggs_bucket_function_timezone)] =
		CStringGetTextDatum(timezone ? timezone : "");

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_insert_values(rel, desc, values, nulls);
	ts_catalog_restore_user(&sec_ctx);
	table_close(rel, RowExclusiveLock);
}

/*
 * Create hypertable for the table referred by mat_tbloid
 * matpartcolname - partition column for hypertable
 * timecol_interval - is the partitioning column's interval for hypertable partition
 */
static void
cagg_create_hypertable(int32 hypertable_id, Oid mat_tbloid, const char *matpartcolname,
					   int64 mat_tbltimecol_interval)
{
	bool created;
	int flags = 0;
	NameData mat_tbltimecol;
	DimensionInfo *time_dim_info;
	ChunkSizingInfo *chunk_sizing_info;
	namestrcpy(&mat_tbltimecol, matpartcolname);
	time_dim_info = ts_dimension_info_create_open(mat_tbloid,
												  &mat_tbltimecol,
												  Int64GetDatum(mat_tbltimecol_interval),
												  INT8OID,
												  InvalidOid);
	/*
	 * Ideally would like to change/expand the API so setting the column name manually is
	 * unnecessary, but not high priority.
	 */
	chunk_sizing_info = ts_chunk_sizing_info_get_default_disabled(mat_tbloid);
	chunk_sizing_info->colname = matpartcolname;
	created = ts_hypertable_create_from_info(mat_tbloid,
											 hypertable_id,
											 flags,
											 time_dim_info,
											 NULL,
											 NULL,
											 NULL,
											 chunk_sizing_info,
											 HYPERTABLE_REGULAR,
											 NULL);
	if (!created)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not create materialization hypertable")));
}

static bool
check_trigger_exists_hypertable(Oid relid, char *trigname)
{
	Relation tgrel;
	ScanKeyData skey[1];
	SysScanDesc tgscan;
	HeapTuple tuple;
	bool trg_found = false;

	tgrel = table_open(TriggerRelationId, AccessShareLock);
	ScanKeyInit(&skey[0],
				Anum_pg_trigger_tgrelid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));

	tgscan = systable_beginscan(tgrel, TriggerRelidNameIndexId, true, NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_trigger trig = (Form_pg_trigger) GETSTRUCT(tuple);
		if (namestrcmp(&(trig->tgname), trigname) == 0)
		{
			trg_found = true;
			break;
		}
	}
	systable_endscan(tgscan);
	table_close(tgrel, AccessShareLock);
	return trg_found;
}

/*
 * Add continuous agg invalidation trigger to hypertable
 * relid - oid of hypertable
 * hypertableid - argument to pass to trigger
 * (the hypertable id from timescaledb catalog)
 */
static void
cagg_add_trigger_hypertable(Oid relid, int32 hypertable_id)
{
	char hypertable_id_str[12];
	ObjectAddress objaddr;
	char *relname = get_rel_name(relid);
	Oid schemaid = get_rel_namespace(relid);
	char *schema = get_namespace_name(schemaid);
	Cache *hcache;
	Hypertable *ht;

	CreateTrigStmt stmt_template = {
		.type = T_CreateTrigStmt,
		.row = true,
		.timing = TRIGGER_TYPE_AFTER,
		.trigname = CAGGINVAL_TRIGGER_NAME,
		.relation = makeRangeVar(schema, relname, -1),
		.funcname =
			list_make2(makeString(INTERNAL_SCHEMA_NAME), makeString(CAGG_INVALIDATION_TRIGGER)),
		.args = NIL, /* to be filled in later */
		.events = TRIGGER_TYPE_INSERT | TRIGGER_TYPE_UPDATE | TRIGGER_TYPE_DELETE,
	};
	if (check_trigger_exists_hypertable(relid, CAGGINVAL_TRIGGER_NAME))
		return;
	ht = ts_hypertable_cache_get_cache_and_entry(relid, CACHE_FLAG_NONE, &hcache);
	if (hypertable_is_distributed(ht))
	{
		DistCmdResult *result;
		List *data_node_list = ts_hypertable_get_data_node_name_list(ht);
		List *cmd_descriptors = NIL; /* same order as ht->data_nodes */
		DistCmdDescr *cmd_descr_data = NULL;
		ListCell *cell;

		unsigned i = 0;
		cmd_descr_data = palloc(list_length(data_node_list) * sizeof(*cmd_descr_data));
		foreach (cell, ht->data_nodes)
		{
			HypertableDataNode *node = lfirst(cell);
			char node_hypertable_id_str[12];
			CreateTrigStmt remote_stmt = stmt_template;

			pg_ltoa(node->fd.node_hypertable_id, node_hypertable_id_str);
			pg_ltoa(node->fd.hypertable_id, hypertable_id_str);

			remote_stmt.args =
				list_make2(makeString(node_hypertable_id_str), makeString(hypertable_id_str));
			cmd_descr_data[i].sql = deparse_create_trigger(&remote_stmt);
			cmd_descr_data[i].params = NULL;
			cmd_descriptors = lappend(cmd_descriptors, &cmd_descr_data[i++]);
		}

		result =
			ts_dist_multi_cmds_params_invoke_on_data_nodes(cmd_descriptors, data_node_list, true);
		if (result)
			ts_dist_cmd_close_response(result);
		/*
		 * FALL-THROUGH
		 * We let the Access Node create a trigger as well, even though it is not used for data
		 * modifications. We use the Access Node trigger as a check for existence of the remote
		 * triggers.
		 */
	}
	CreateTrigStmt local_stmt = stmt_template;
	pg_ltoa(hypertable_id, hypertable_id_str);
	local_stmt.args = list_make1(makeString(hypertable_id_str));
	objaddr = ts_hypertable_create_trigger(ht, &local_stmt, NULL);
	if (!OidIsValid(objaddr.objectId))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("could not create continuous aggregate trigger")));
	ts_cache_release(hcache);
}

/*
 * Add additional indexes to materialization table for the columns derived from
 * the group-by column list of the partial select query.
 * If partial select query has:
 * GROUP BY timebucket_expr, <grpcol1, grpcol2, grpcol3 ...>
 * index on mattable is <grpcol1, timebucketcol>, <grpcol2, timebucketcol> ... and so on.
 * i.e. #indexes =(  #grp-cols - 1)
 */
static void
mattablecolumninfo_add_mattable_index(MatTableColumnInfo *matcolinfo, Hypertable *ht)
{
	IndexStmt stmt = {
		.type = T_IndexStmt,
		.accessMethod = DEFAULT_INDEX_TYPE,
		.idxname = NULL,
		.relation = makeRangeVar(NameStr(ht->fd.schema_name), NameStr(ht->fd.table_name), 0),
		.tableSpace = get_tablespace_name(get_rel_tablespace(ht->main_table_relid)),
	};
	IndexElem timeelem = { .type = T_IndexElem,
						   .name = matcolinfo->matpartcolname,
						   .ordering = SORTBY_DESC };
	ListCell *le = NULL;
	foreach (le, matcolinfo->mat_groupcolname_list)
	{
		NameData indxname;
		ObjectAddress indxaddr;
		HeapTuple indxtuple;
		char *grpcolname = (char *) lfirst(le);
		IndexElem grpelem = { .type = T_IndexElem, .name = grpcolname };
		stmt.indexParams = list_make2(&grpelem, &timeelem);
		indxaddr = DefineIndex(ht->main_table_relid,
							   &stmt,
							   InvalidOid, /* indexRelationId */
							   InvalidOid, /* parentIndexId */
							   InvalidOid, /* parentConstraintId */
							   false,	   /* is_alter_table */
							   false,	   /* check_rights */
							   false,	   /* check_not_in_use */
							   false,	   /* skip_build */
							   false);	   /* quiet */
		indxtuple = SearchSysCache1(RELOID, ObjectIdGetDatum(indxaddr.objectId));

		if (!HeapTupleIsValid(indxtuple))
			elog(ERROR, "cache lookup failed for index relid %u", indxaddr.objectId);
		indxname = ((Form_pg_class) GETSTRUCT(indxtuple))->relname;
		elog(DEBUG1,
			 "adding index %s ON %s.%s USING BTREE(%s, %s)",
			 NameStr(indxname),
			 NameStr(ht->fd.schema_name),
			 NameStr(ht->fd.table_name),
			 grpcolname,
			 matcolinfo->matpartcolname);
		ReleaseSysCache(indxtuple);
	}
}

/*
 * Create the materialization hypertable root by faking up a
 * CREATE TABLE parsetree and passing it to DefineRelation.
 * Reuse the information from ViewStmt:
 *   Remove the options on the into clause that we will not honour
 *   Modify the relname to ts_internal_<name>
 *
 *  Parameters:
 *    mat_rel: relation information for the materialization table
 *    origquery_tblinfo: - user query's tbale information. used for setting up
 *        thr partitioning of the hypertable.
 *    tablespace_name: Name of the tablespace for the materialization table.
 *    table_access_method: Name of the table access method to use for the
 *        materialization table.
 *    mataddress: return the ObjectAddress RETURNS: hypertable id of the
 *        materialization table
 */
static int32
mattablecolumninfo_create_materialization_table(MatTableColumnInfo *matcolinfo, int32 hypertable_id,
												RangeVar *mat_rel,
												CAggTimebucketInfo *origquery_tblinfo,
												bool create_addl_index, char *const tablespacename,
												char *const table_access_method,
												ObjectAddress *mataddress)
{
	Oid uid, saved_uid;
	int sec_ctx;
	char *matpartcolname = matcolinfo->matpartcolname;
	CreateStmt *create;
	Datum toast_options;
	int64 matpartcol_interval;
	static char *validnsps[] = HEAP_RELOPT_NAMESPACES;
	int32 mat_htid;
	Oid mat_relid;
	Cache *hcache;
	Hypertable *mat_ht = NULL, *orig_ht = NULL;
	Oid owner = GetUserId();

	create = makeNode(CreateStmt);
	create->relation = mat_rel;
	create->tableElts = matcolinfo->matcollist;
	create->inhRelations = NIL;
	create->ofTypename = NULL;
	create->constraints = NIL;
	create->options = NULL;
	create->oncommit = ONCOMMIT_NOOP;
	create->tablespacename = tablespacename;
	create->accessMethod = table_access_method;
	create->if_not_exists = false;

	/*  Create the materialization table.  */
	SWITCH_TO_TS_USER(mat_rel->schemaname, uid, saved_uid, sec_ctx);
	*mataddress = DefineRelation(create, RELKIND_RELATION, owner, NULL, NULL);
	CommandCounterIncrement();
	mat_relid = mataddress->objectId;

	/* NewRelationCreateToastTable calls CommandCounterIncrement. */
	toast_options =
		transformRelOptions((Datum) 0, create->options, "toast", validnsps, true, false);
	(void) heap_reloptions(RELKIND_TOASTVALUE, toast_options, true);
	NewRelationCreateToastTable(mat_relid, toast_options);
	RESTORE_USER(uid, saved_uid, sec_ctx);

	/* Convert the materialization table to a hypertable. */
	matpartcol_interval = MATPARTCOL_INTERVAL_FACTOR * (origquery_tblinfo->htpartcol_interval_len);
	cagg_create_hypertable(hypertable_id, mat_relid, matpartcolname, matpartcol_interval);

	/* Retrieve the hypertable id from the cache. */
	mat_ht = ts_hypertable_cache_get_cache_and_entry(mat_relid, CACHE_FLAG_NONE, &hcache);
	mat_htid = mat_ht->fd.id;

	/* Create additional index on the group-by columns for the materialization table. */
	if (create_addl_index)
		mattablecolumninfo_add_mattable_index(matcolinfo, mat_ht);

	/*
	 * Initialize the invalidation log for the cagg. Initially, everything is
	 * invalid. Add an infinite invalidation for the continuous
	 * aggregate. This is the initial state of the aggregate before any
	 * refreshes.
	 */
	orig_ht = ts_hypertable_cache_get_entry(hcache, origquery_tblinfo->htoid, CACHE_FLAG_NONE);
	continuous_agg_invalidate_mat_ht(orig_ht, mat_ht, TS_TIME_NOBEGIN, TS_TIME_NOEND);
	ts_cache_release(hcache);
	return mat_htid;
}

/*
 * Use the userview query to create the partial query to populate
 * the materialization columns and remove HAVING clause and ORDER BY.
 */
static Query *
mattablecolumninfo_get_partial_select_query(MatTableColumnInfo *mattblinfo, Query *userview_query,
											bool finalized)
{
	Query *partial_selquery;

	CAGG_MAKEQUERY(partial_selquery, userview_query);
	partial_selquery->rtable = copyObject(userview_query->rtable);
	partial_selquery->jointree = copyObject(userview_query->jointree);

	partial_selquery->targetList = mattblinfo->partial_seltlist;
	partial_selquery->groupClause = mattblinfo->partial_grouplist;

	if (finalized)
	{
		partial_selquery->havingQual = copyObject(userview_query->havingQual);
		partial_selquery->sortClause = copyObject(userview_query->sortClause);
	}
	else
	{
		partial_selquery->havingQual = NULL;
		partial_selquery->sortClause = NULL;
	}

	return partial_selquery;
}

/*
 * Create a view for the query using the SELECt stmt sqlquery
 * and view name from RangeVar viewrel.
 */
static ObjectAddress
create_view_for_query(Query *selquery, RangeVar *viewrel)
{
	Oid uid, saved_uid;
	int sec_ctx;
	ObjectAddress address;
	CreateStmt *create;
	List *selcollist = NIL;
	Oid owner = GetUserId();
	ListCell *lc;
	foreach (lc, selquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		if (!tle->resjunk)
		{
			ColumnDef *col = makeColumnDef(tle->resname,
										   exprType((Node *) tle->expr),
										   exprTypmod((Node *) tle->expr),
										   exprCollation((Node *) tle->expr));
			selcollist = lappend(selcollist, col);
		}
	}

	create = makeNode(CreateStmt);
	create->relation = viewrel;
	create->tableElts = selcollist;
	create->inhRelations = NIL;
	create->ofTypename = NULL;
	create->constraints = NIL;
	create->options = NULL;
	create->oncommit = ONCOMMIT_NOOP;
	create->tablespacename = NULL;
	create->if_not_exists = false;

	/*
	 * Create the view. Viewname is in viewrel.
	 */
	SWITCH_TO_TS_USER(viewrel->schemaname, uid, saved_uid, sec_ctx);
	address = DefineRelation(create, RELKIND_VIEW, owner, NULL, NULL);
	CommandCounterIncrement();
	StoreViewQuery(address.objectId, selquery, false);
	CommandCounterIncrement();
	RESTORE_USER(uid, saved_uid, sec_ctx);
	return address;
}

/*
 * Initialize caggtimebucket.
 */
static void
caggtimebucketinfo_init(CAggTimebucketInfo *src, int32 hypertable_id, Oid hypertable_oid,
						AttrNumber hypertable_partition_colno, Oid hypertable_partition_coltype,
						int64 hypertable_partition_col_interval, int32 parent_mat_hypertable_id)
{
	src->htid = hypertable_id;
	src->parent_mat_hypertable_id = parent_mat_hypertable_id;
	src->htoid = hypertable_oid;
	src->htpartcolno = hypertable_partition_colno;
	src->htpartcoltype = hypertable_partition_coltype;
	src->htpartcol_interval_len = hypertable_partition_col_interval;
	src->bucket_width = 0;				 /* invalid value */
	src->bucket_width_type = InvalidOid; /* invalid oid */
	src->interval = NULL;				 /* not specified by default */
	src->timezone = NULL;				 /* not specified by default */
	TIMESTAMP_NOBEGIN(src->origin);		 /* origin is not specified by default */
}

static Const *
check_time_bucket_argument(Node *arg, char *position)
{
	if (IsA(arg, NamedArgExpr))
		arg = (Node *) castNode(NamedArgExpr, arg)->arg;

	Node *expr = eval_const_expressions(NULL, arg);

	if (!IsA(expr, Const))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only immutable expressions allowed in time bucket function"),
				 errhint("Use an immutable expression as %s argument to the time bucket function.",
						 position)));

	return castNode(Const, expr);
}

/*
 * Check if the group-by clauses has exactly 1 time_bucket(.., <col>) where
 * <col> is the hypertable's partitioning column and other invariants. Then fill
 * the `bucket_width` and other fields of `tbinfo`.
 */
static void
caggtimebucket_validate(CAggTimebucketInfo *tbinfo, List *groupClause, List *targetList)
{
	ListCell *l;
	bool found = false;
	bool custom_origin = false;

	/* Make sure tbinfo was initialized. This assumption is used below. */
	Assert(tbinfo->bucket_width == 0);
	Assert(tbinfo->timezone == NULL);
	Assert(TIMESTAMP_NOT_FINITE(tbinfo->origin));

	foreach (l, groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(l);
		TargetEntry *tle = get_sortgroupclause_tle(sgc, targetList);

		if (IsA(tle->expr, FuncExpr))
		{
			FuncExpr *fe = ((FuncExpr *) tle->expr);
			Node *width_arg;
			Node *col_arg;

			if (!function_allowed_in_cagg_definition(fe->funcid))
				continue;

			/*
			 * Offset variants of time_bucket functions are not
			 * supported at the moment.
			 */
			if (list_length(fe->args) >= 5 ||
				(list_length(fe->args) == 4 && exprType(lfourth(fe->args)) == INTERVALOID))
				continue;

			if (found)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("continuous aggregate view cannot contain"
								" multiple time bucket functions")));
			else
				found = true;

			tbinfo->bucket_func = fe;

			/* Only column allowed : time_bucket('1day', <column> ) */
			col_arg = lsecond(fe->args);

			if (!(IsA(col_arg, Var)) || ((Var *) col_arg)->varattno != tbinfo->htpartcolno)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg(
							 "time bucket function must reference a hypertable dimension column")));

			if (list_length(fe->args) >= 3)
			{
				Const *arg = check_time_bucket_argument(lthird(fe->args), "third");
				if (exprType((Node *) arg) == TEXTOID)
				{
					const char *tz_name = TextDatumGetCString(arg->constvalue);
					if (!ts_is_valid_timezone_name(tz_name))
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid timezone name \"%s\"", tz_name)));
					}

					tbinfo->timezone = tz_name;
					tbinfo->bucket_width = BUCKET_WIDTH_VARIABLE;
				}
			}

			if (list_length(fe->args) >= 4)
			{
				Const *arg = check_time_bucket_argument(lfourth(fe->args), "fourth");
				if (exprType((Node *) arg) == TEXTOID)
				{
					const char *tz_name = TextDatumGetCString(arg->constvalue);
					if (!ts_is_valid_timezone_name(tz_name))
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("invalid timezone name \"%s\"", tz_name)));
					}

					tbinfo->timezone = tz_name;
					tbinfo->bucket_width = BUCKET_WIDTH_VARIABLE;
				}
			}

			/* Check for custom origin. */
			switch (exprType(col_arg))
			{
				case DATEOID:
					/* Origin is always 3rd arg for date variants. */
					if (list_length(fe->args) == 3)
					{
						custom_origin = true;
						tbinfo->origin = DatumGetTimestamp(
							DirectFunctionCall1(date_timestamp,
												castNode(Const, lthird(fe->args))->constvalue));
					}
					break;
				case TIMESTAMPOID:
					/* Origin is always 3rd arg for timestamp variants. */
					if (list_length(fe->args) == 3)
					{
						custom_origin = true;
						tbinfo->origin =
							DatumGetTimestamp(castNode(Const, lthird(fe->args))->constvalue);
					}
					break;
				case TIMESTAMPTZOID:
					/* Origin can be 3rd or 4th arg for timestamptz variants. */
					if (list_length(fe->args) >= 3 && exprType(lthird(fe->args)) == TIMESTAMPTZOID)
					{
						custom_origin = true;
						tbinfo->origin =
							DatumGetTimestampTz(castNode(Const, lthird(fe->args))->constvalue);
					}
					else if (list_length(fe->args) >= 4 &&
							 exprType(lfourth(fe->args)) == TIMESTAMPTZOID)
					{
						custom_origin = true;
						tbinfo->origin =
							DatumGetTimestampTz(castNode(Const, lfourth(fe->args))->constvalue);
					}
			}
			if (custom_origin && TIMESTAMP_NOT_FINITE(tbinfo->origin))
			{
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("invalid origin value: infinity")));
			}

			/*
			 * We constify width expression here so any immutable expression will be allowed.
			 * Otherwise it would make it harder to create caggs for hypertables with e.g. int8
			 * partitioning column as int constants default to int4 and so expression would
			 * have a cast and not be a Const.
			 */
			width_arg = eval_const_expressions(NULL, linitial(fe->args));
			if (IsA(width_arg, Const))
			{
				Const *width = castNode(Const, width_arg);
				tbinfo->bucket_width_type = width->consttype;

				if (width->consttype == INTERVALOID)
				{
					tbinfo->interval = DatumGetIntervalP(width->constvalue);
					if (tbinfo->interval->month != 0)
						tbinfo->bucket_width = BUCKET_WIDTH_VARIABLE;
				}

				if (tbinfo->bucket_width != BUCKET_WIDTH_VARIABLE)
				{
					/* The bucket size is fixed. */
					tbinfo->bucket_width =
						ts_interval_value_to_internal(width->constvalue, width->consttype);
				}
			}
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("only immutable expressions allowed in time bucket function"),
						 errhint("Use an immutable expression as first argument"
								 " to the time bucket function.")));

			if (tbinfo->interval && tbinfo->interval->month)
			{
				tbinfo->bucket_width = BUCKET_WIDTH_VARIABLE;
			}
		}
	}

	if (tbinfo->bucket_width == BUCKET_WIDTH_VARIABLE)
	{
		/* Variable-sized buckets can be used only with intervals. */
		Assert(tbinfo->interval != NULL);

		if ((tbinfo->interval->month != 0) &&
			((tbinfo->interval->day != 0) || (tbinfo->interval->time != 0)))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("invalid interval specified"),
					 errhint("Use either months or days and hours, but not months, days and hours "
							 "together")));
		}
	}

	if (!found)
		elog(ERROR, "continuous aggregate view must include a valid time bucket function");
}

static bool
cagg_agg_validate(Node *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Aggref))
	{
		Aggref *agg = (Aggref *) node;
		HeapTuple aggtuple;
		Form_pg_aggregate aggform;
		if (agg->aggorder || agg->aggdistinct || agg->aggfilter)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregates with FILTER / DISTINCT / ORDER BY are not supported")));
		}
		/* Fetch the pg_aggregate row. */
		aggtuple = SearchSysCache1(AGGFNOID, agg->aggfnoid);
		if (!HeapTupleIsValid(aggtuple))
			elog(ERROR, "cache lookup failed for aggregate %u", agg->aggfnoid);
		aggform = (Form_pg_aggregate) GETSTRUCT(aggtuple);
		if (aggform->aggkind != 'n')
		{
			ReleaseSysCache(aggtuple);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("ordered set/hypothetical aggregates are not supported")));
		}
		if (!OidIsValid(aggform->aggcombinefn) ||
			(aggform->aggtranstype == INTERNALOID && !OidIsValid(aggform->aggdeserialfn)))
		{
			ReleaseSysCache(aggtuple);
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("aggregates which are not parallelizable are not supported")));
		}
		ReleaseSysCache(aggtuple);

		return false;
	}
	return expression_tree_walker(node, cagg_agg_validate, context);
}

/*
 * Check query and extract error details and error hints.
 *
 * Returns:
 *   True if the query is supported, false otherwise with hints and errors
 *   added.
 */
static bool
cagg_query_supported(const Query *query, StringInfo hint, StringInfo detail, const bool finalized)
{
/*
 * For now deprecate partial aggregates on release builds only.
 * Once migration tests are made compatible with PG15 enable deprecation
 * on debug builds as well.
 */
#ifndef DEBUG
#if PG15_GE
	if (!finalized)
	{
		/* continuous aggregates with old format will not be allowed */
		appendStringInfoString(detail,
							   "Continuous Aggregates with partials is not supported anymore.");
		appendStringInfoString(hint,
							   "Define the Continuous Aggregate with \"finalized\" parameter set "
							   "to true.");
		return false;
	}
#endif
#endif

	if (query->commandType != CMD_SELECT)
	{
		appendStringInfoString(hint, "Use a SELECT query in the continuous aggregate view.");
		return false;
	}

	if (query->hasWindowFuncs)
	{
		appendStringInfoString(detail,
							   "Window functions are not supported by continuous aggregates.");
		return false;
	}

	if (query->hasDistinctOn || query->distinctClause)
	{
		appendStringInfoString(detail,
							   "DISTINCT / DISTINCT ON queries are not supported by continuous "
							   "aggregates.");
		return false;
	}

	if (query->limitOffset || query->limitCount)
	{
		appendStringInfoString(detail,
							   "LIMIT and LIMIT OFFSET are not supported in queries defining "
							   "continuous aggregates.");
		appendStringInfoString(hint,
							   "Use LIMIT and LIMIT OFFSET in SELECTS from the continuous "
							   "aggregate view instead.");
		return false;
	}

	if (query->sortClause && !finalized)
	{
		appendStringInfoString(detail,
							   "ORDER BY is not supported in queries defining continuous "
							   "aggregates.");
		appendStringInfoString(hint,
							   "Use ORDER BY clauses in SELECTS from the continuous aggregate view "
							   "instead.");
		return false;
	}

	if (query->hasRecursive || query->hasSubLinks || query->hasTargetSRFs || query->cteList)
	{
		appendStringInfoString(detail,
							   "CTEs, subqueries and set-returning functions are not supported by "
							   "continuous aggregates.");
		return false;
	}

	if (query->hasForUpdate || query->hasModifyingCTE)
	{
		appendStringInfoString(detail,
							   "Data modification is not allowed in continuous aggregate view "
							   "definitions.");
		return false;
	}

	if (query->hasRowSecurity)
	{
		appendStringInfoString(detail,
							   "Row level security is not supported by continuous aggregate "
							   "views.");
		return false;
	}

	if (query->groupingSets)
	{
		appendStringInfoString(detail,
							   "GROUP BY GROUPING SETS, ROLLUP and CUBE are not supported by "
							   "continuous aggregates");
		appendStringInfoString(hint,
							   "Define multiple continuous aggregates with different grouping "
							   "levels.");
		return false;
	}

	if (query->setOperations)
	{
		appendStringInfoString(detail,
							   "UNION, EXCEPT & INTERSECT are not supported by continuous "
							   "aggregates");
		return false;
	}

	if (!query->groupClause)
	{
		/*
		 * Query can have aggregate without group by , so look
		 * for groupClause.
		 */
		appendStringInfoString(hint,
							   "Include at least one aggregate function"
							   " and a GROUP BY clause with time bucket.");
		return false;
	}

	return true; /* Query was OK and is supported. */
}

static inline int64
get_bucket_width(CAggTimebucketInfo bucket_info)
{
	int64 width = 0;

	/* Calculate the width. */
	switch (bucket_info.bucket_width_type)
	{
		case INT8OID:
		case INT4OID:
		case INT2OID:
			width = bucket_info.bucket_width;
			break;
		case INTERVALOID:
		{
			/*
			 * epoch will treat year as 365.25 days. This leads to the unexpected
			 * result that year is not multiple of day or month, which is perceived
			 * as a bug. For that reason, we treat all months as 30 days regardless of year
			 */
			if (bucket_info.interval->month && !bucket_info.interval->day &&
				!bucket_info.interval->time)
			{
				bucket_info.interval->day = bucket_info.interval->month * DAYS_PER_MONTH;
				bucket_info.interval->month = 0;
			}
			Datum epoch = DirectFunctionCall2(interval_part,
											  PointerGetDatum(cstring_to_text("epoch")),
											  IntervalPGetDatum(bucket_info.interval));
			/* Cast float8 to int8. */
			width = DatumGetInt64(DirectFunctionCall1(dtoi8, epoch));
			break;
		}
		default:
			Assert(false);
	}

	return width;
}

static inline Datum
get_bucket_width_datum(CAggTimebucketInfo bucket_info)
{
	Datum width = (Datum) 0;

	switch (bucket_info.bucket_width_type)
	{
		case INT8OID:
		case INT4OID:
		case INT2OID:
			width = ts_internal_to_interval_value(bucket_info.bucket_width,
												  bucket_info.bucket_width_type);
			break;
		case INTERVALOID:
			width = IntervalPGetDatum(bucket_info.interval);
			break;
		default:
			Assert(false);
	}

	return width;
}

static CAggTimebucketInfo
cagg_validate_query(const Query *query, const bool finalized, const char *cagg_schema,
					const char *cagg_name)
{
	CAggTimebucketInfo bucket_info, bucket_info_parent;
	Cache *hcache;
	Hypertable *ht = NULL, *ht_parent = NULL;
	RangeTblRef *rtref = NULL, *rtref_other = NULL;
	RangeTblEntry *rte = NULL, *rte_other = NULL;
	JoinType jointype = JOIN_FULL;
	OpExpr *op = NULL;
	List *fromList = NIL;
	StringInfo hint = makeStringInfo();
	StringInfo detail = makeStringInfo();
	bool is_nested = false;
	Query *prev_query = NULL;
	ContinuousAgg *cagg_parent = NULL;
	Oid normal_table_id = InvalidOid;

	if (!cagg_query_supported(query, hint, detail, finalized))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid continuous aggregate query"),
				 hint->len > 0 ? errhint("%s", hint->data) : 0,
				 detail->len > 0 ? errdetail("%s", detail->data) : 0));
	}

	/* Finalized cagg doesn't have those restrictions anymore. */
	if (!finalized)
	{
		/* Validate aggregates allowed. */
		cagg_agg_validate((Node *) query->targetList, NULL);
		cagg_agg_validate((Node *) query->havingQual, NULL);
	}
	/* Check if there are only two tables in the from list. */
	fromList = query->jointree->fromlist;
	if (list_length(fromList) > CONTINUOUS_AGG_MAX_JOIN_RELATIONS)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only two tables with one hypertable and one normal table"
						"are  allowed in continuous aggregate view")));
	}
	/* Extra checks for joins in Caggs. */
	if (list_length(fromList) == CONTINUOUS_AGG_MAX_JOIN_RELATIONS ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef))
	{
		/* Using old format caggs is not supported */
		if (!finalized)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("old format of continuous aggregate is not supported with joins"),
					 errhint("set timescaledb.finalized to TRUE")));

		if (list_length(fromList) == CONTINUOUS_AGG_MAX_JOIN_RELATIONS)
		{
			if (!IsA(linitial(fromList), RangeTblRef) || !IsA(lsecond(fromList), RangeTblRef))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("invalid continuous aggregate view"),
						 errdetail(
							 "from clause can only have one hypertable and one normal table")));

			rtref = linitial_node(RangeTblRef, query->jointree->fromlist);
			rte = list_nth(query->rtable, rtref->rtindex - 1);
			rtref_other = lsecond_node(RangeTblRef, query->jointree->fromlist);
			rte_other = list_nth(query->rtable, rtref_other->rtindex - 1);
			jointype = rte->jointype || rte_other->jointype;

			if (query->jointree->quals != NULL && IsA(query->jointree->quals, OpExpr))
				op = (OpExpr *) query->jointree->quals;
		}
		else
		{
			ListCell *l;
			foreach (l, query->jointree->fromlist)
			{
				Node *jtnode = (Node *) lfirst(l);
				JoinExpr *join = NULL;
				if (IsA(jtnode, JoinExpr))
				{
					join = castNode(JoinExpr, jtnode);
#if PG13_LT
					if (join->usingClause != NULL)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("invalid continuous aggregate view"),
								 errdetail(
									 "joins with using clause in continuous aggregate definition"
									 " work for Postgres versions 13 and above")));
#endif
					jointype = join->jointype;
					op = (OpExpr *) join->quals;
					rte = list_nth(query->rtable, ((RangeTblRef *) join->larg)->rtindex - 1);
					rte_other = list_nth(query->rtable, ((RangeTblRef *) join->rarg)->rtindex - 1);
				}
			}
		}

		/*
		 * Cagg with joins does not support hierarchical caggs in from clause.
		 */
		if (rte->relkind == RELKIND_VIEW || rte_other->relkind == RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("joins for hierarchical continuous aggregates are not supported")));

		/*
		 * Error out if there is aynthing else than one normal table and one hypertable
		 * in the from clause, e.g. sub-query.
		 */
		if (((rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_VIEW) ||
			 rte->tablesample || rte->inh == false) ||
			((rte_other->relkind != RELKIND_RELATION && rte_other->relkind != RELKIND_VIEW) ||
			 rte_other->tablesample || rte_other->inh == false) ||
			(ts_is_hypertable(rte->relid) == ts_is_hypertable(rte_other->relid)))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("invalid continuous aggregate view"),
					 errdetail("from clause can only have one hypertable and one normal table")));

		/* Only inner joins are allowed. */
		if (jointype != JOIN_INNER)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only inner joins are supported in continuous aggregates")));

		/* Only equality conditions are permitted on joins. */
		if (op && IsA(op, OpExpr) &&
			list_length(castNode(OpExpr, op)->args) == CONTINUOUS_AGG_MAX_JOIN_RELATIONS)
		{
			Oid left_type = exprType(linitial(op->args));
			Oid right_type = exprType(lsecond(op->args));
			if (!ts_is_equality_operator(op->opno, left_type, right_type))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("invalid continuous aggregate view"),
						 errdetail(
							 "only equality conditions are supported in continuous aggregates")));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("invalid continuous aggregate view"),
					 errdetail("unsupported expression in join clause"),
					 errhint("only equality condition is supported")));
		/*
		 * Record the table oid of the normal table. This is required so
		 * that we know which one is hypertable to carry out the related
		 * processing in later parts of code.
		 */
		normal_table_id = ts_is_hypertable(rte->relid) ? rte_other->relid : rte->relid;
		if (normal_table_id == rte->relid)
			rte = rte_other;
	}
	else
	{
		/* Check if we have a hypertable in the FROM clause. */
		rtref = linitial_node(RangeTblRef, query->jointree->fromlist);
		rte = list_nth(query->rtable, rtref->rtindex - 1);
	}
	/* FROM only <tablename> sets rte->inh to false. */
	if (rte->rtekind != RTE_JOIN)
	{
		if ((rte->relkind != RELKIND_RELATION && rte->relkind != RELKIND_VIEW) ||
			rte->tablesample || rte->inh == false)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("invalid continuous aggregate view")));
	}

	if (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_VIEW)
	{
		const Dimension *part_dimension = NULL;
		int32 parent_mat_hypertable_id = INVALID_HYPERTABLE_ID;

		if (rte->relkind == RELKIND_RELATION)
			ht = ts_hypertable_cache_get_cache_and_entry(rte->relid, CACHE_FLAG_NONE, &hcache);
		else
		{
			cagg_parent = ts_continuous_agg_find_by_relid(rte->relid);

			if (!cagg_parent)
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("invalid continuous aggregate query"),
						 errhint("continuous aggregate needs to query hypertable or another "
								 "continuous aggregate")));
			}

			if (!ContinuousAggIsFinalized(cagg_parent))
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("old format of continuous aggregate is not supported"),
						 errhint("Run \"CALL cagg_migrate('%s.%s');\" to migrate to the new "
								 "format.",
								 NameStr(cagg_parent->data.user_view_schema),
								 NameStr(cagg_parent->data.user_view_name))));
			}

			parent_mat_hypertable_id = cagg_parent->data.mat_hypertable_id;
			hcache = ts_hypertable_cache_pin();
			ht = ts_hypertable_cache_get_entry_by_id(hcache, cagg_parent->data.mat_hypertable_id);

			/* If parent cagg is nested then we should get the matht otherwise the rawht. */
			if (ContinuousAggIsNested(cagg_parent))
				ht_parent =
					ts_hypertable_cache_get_entry_by_id(hcache,
														cagg_parent->data.mat_hypertable_id);
			else
				ht_parent =
					ts_hypertable_cache_get_entry_by_id(hcache,
														cagg_parent->data.raw_hypertable_id);

			/* Get the querydef for the source cagg. */
			is_nested = true;
			prev_query = ts_continuous_agg_get_query(cagg_parent);
		}

		if (TS_HYPERTABLE_IS_INTERNAL_COMPRESSION_TABLE(ht))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("hypertable is an internal compressed hypertable")));

		if (rte->relkind == RELKIND_RELATION)
		{
			ContinuousAggHypertableStatus status = ts_continuous_agg_hypertable_status(ht->fd.id);

			/* Prevent create a CAGG over an existing materialization hypertable. */
			if (status == HypertableIsMaterialization ||
				status == HypertableIsMaterializationAndRaw)
			{
				const ContinuousAgg *cagg = ts_continuous_agg_find_by_mat_hypertable_id(ht->fd.id);
				Assert(cagg != NULL);

				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("hypertable is a continuous aggregate materialization table"),
						 errdetail("Materialization hypertable \"%s.%s\".",
								   NameStr(ht->fd.schema_name),
								   NameStr(ht->fd.table_name)),
						 errhint("Do you want to use continuous aggregate \"%s.%s\" instead?",
								 NameStr(cagg->data.user_view_schema),
								 NameStr(cagg->data.user_view_name))));
			}
		}

		/* Get primary partitioning column information. */
		part_dimension = hyperspace_get_open_dimension(ht->space, 0);

		/*
		 * NOTE: if we ever allow custom partitioning functions we'll need to
		 *       change part_dimension->fd.column_type to partitioning_type
		 *       below, along with any other fallout.
		 */
		if (part_dimension->partitioning != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("custom partitioning functions not supported"
							" with continuous aggregates")));

		if (IS_INTEGER_TYPE(ts_dimension_get_partition_type(part_dimension)) &&
			rte->relkind == RELKIND_RELATION)
		{
			const char *funcschema = NameStr(part_dimension->fd.integer_now_func_schema);
			const char *funcname = NameStr(part_dimension->fd.integer_now_func);

			if (strlen(funcschema) == 0 || strlen(funcname) == 0)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("custom time function required on hypertable \"%s\"",
								get_rel_name(ht->main_table_relid)),
						 errdetail("An integer-based hypertable requires a custom time"
								   " function to support continuous aggregates."),
						 errhint("Set a custom time function on the hypertable.")));
		}

		caggtimebucketinfo_init(&bucket_info,
								ht->fd.id,
								ht->main_table_relid,
								part_dimension->column_attno,
								part_dimension->fd.column_type,
								part_dimension->fd.interval_length,
								parent_mat_hypertable_id);

		if (is_nested)
		{
			const Dimension *part_dimension_parent =
				hyperspace_get_open_dimension(ht_parent->space, 0);

			caggtimebucketinfo_init(&bucket_info_parent,
									ht_parent->fd.id,
									ht_parent->main_table_relid,
									part_dimension_parent->column_attno,
									part_dimension_parent->fd.column_type,
									part_dimension_parent->fd.interval_length,
									INVALID_HYPERTABLE_ID);
		}

		ts_cache_release(hcache);
	}

	/* Check row security settings for the table. */
	if (ts_has_row_security(rte->relid))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot create continuous aggregate on hypertable with row security")));

	/*
	 * We need a GROUP By clause with time_bucket on the partitioning
	 * column of the hypertable.
	 */
	Assert(query->groupClause);
	caggtimebucket_validate(&bucket_info, query->groupClause, query->targetList);

	/* Nested cagg validations. */
	if (is_nested)
	{
		int64 bucket_width = 0, bucket_width_parent = 0;
		bool is_greater_or_equal_than_parent = true, is_multiple_of_parent = true;

		Assert(prev_query->groupClause);
		caggtimebucket_validate(&bucket_info_parent,
								prev_query->groupClause,
								prev_query->targetList);

		/* Cannot create cagg with fixed bucket on top of variable bucket. */
		if ((bucket_info_parent.bucket_width == BUCKET_WIDTH_VARIABLE &&
			 bucket_info.bucket_width != BUCKET_WIDTH_VARIABLE))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create continuous aggregate with fixed-width bucket on top of "
							"one using variable-width bucket"),
					 errdetail("Continuous aggregate with a fixed time bucket width (e.g. 61 days) "
							   "cannot be created on top of one using variable time bucket width "
							   "(e.g. 1 month).\n"
							   "The variance can lead to the fixed width one not being a multiple "
							   "of the variable width one.")));
		}

		/* Get bucket widths for validation. */
		bucket_width = get_bucket_width(bucket_info);
		bucket_width_parent = get_bucket_width(bucket_info_parent);

		Assert(bucket_width != 0);
		Assert(bucket_width_parent != 0);

		/* Check if the current bucket is greater or equal than the parent. */
		is_greater_or_equal_than_parent = (bucket_width >= bucket_width_parent);

		/* Check if buckets are multiple. */
		if (bucket_width_parent != 0)
		{
			if (bucket_width_parent > bucket_width && bucket_width != 0)
				is_multiple_of_parent = ((bucket_width_parent % bucket_width) == 0);
			else
				is_multiple_of_parent = ((bucket_width % bucket_width_parent) == 0);
		}

		/* Proceed with validation errors. */
		if (!is_greater_or_equal_than_parent || !is_multiple_of_parent)
		{
			Datum width, width_parent;
			Oid outfuncid = InvalidOid;
			bool isvarlena;
			char *width_out, *width_out_parent;
			char *message = NULL;

			getTypeOutputInfo(bucket_info.bucket_width_type, &outfuncid, &isvarlena);
			width = get_bucket_width_datum(bucket_info);
			width_out = DatumGetCString(OidFunctionCall1(outfuncid, width));

			getTypeOutputInfo(bucket_info_parent.bucket_width_type, &outfuncid, &isvarlena);
			width_parent = get_bucket_width_datum(bucket_info_parent);
			width_out_parent = DatumGetCString(OidFunctionCall1(outfuncid, width_parent));

			/* New bucket should be multiple of the parent. */
			if (!is_multiple_of_parent)
				message = "multiple of";

			/* New bucket should be greater than the parent. */
			if (!is_greater_or_equal_than_parent)
				message = "greater or equal than";

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot create continuous aggregate with incompatible bucket width"),
					 errdetail("Time bucket width of \"%s.%s\" [%s] should be %s the time "
							   "bucket width of \"%s.%s\" [%s].",
							   cagg_schema,
							   cagg_name,
							   width_out,
							   message,
							   NameStr(cagg_parent->data.user_view_schema),
							   NameStr(cagg_parent->data.user_view_name),
							   width_out_parent)));
		}
	}

	return bucket_info;
}

/*
 * Add ts_internal_cagg_final to bytea column.
 * Bytea column is the internal state for an agg.
 * Pass info for the agg as "inp".
 * inpcol = bytea column.
 * This function returns an aggref
 * ts_internal_cagg_final( Oid, Oid, bytea, NULL::output_typeid)
 * the arguments are a list of targetentry.
 */
static Oid
get_finalizefnoid()
{
	Oid finalfnoid;
	Oid finalfnargtypes[] = { TEXTOID,	NAMEOID,	  NAMEOID, get_array_type(NAMEOID),
							  BYTEAOID, ANYELEMENTOID };
	List *funcname = list_make2(makeString(INTERNAL_SCHEMA_NAME), makeString(FINALFN));
	int nargs = sizeof(finalfnargtypes) / sizeof(finalfnargtypes[0]);
	finalfnoid = LookupFuncName(funcname, nargs, finalfnargtypes, false);
	return finalfnoid;
}

/*
 * Build a [N][2] array where N is number of arguments
 * and the inner array is of [schema_name,type_name].
 */
static Datum
get_input_types_array_datum(Aggref *original_aggregate)
{
	ListCell *lc;
	MemoryContext builder_context =
		AllocSetContextCreate(CurrentMemoryContext, "input types builder", ALLOCSET_DEFAULT_SIZES);
	Oid name_array_type_oid = get_array_type(NAMEOID);
	ArrayBuildStateArr *outer_builder =
		initArrayResultArr(name_array_type_oid, NAMEOID, builder_context, false);
	Datum result;

	foreach (lc, original_aggregate->args)
	{
		TargetEntry *te = lfirst(lc);
		Oid type_oid = exprType((Node *) te->expr);
		ArrayBuildState *schema_name_builder = initArrayResult(NAMEOID, builder_context, false);
		HeapTuple tp;
		Form_pg_type typtup;
		char *schema_name;
		Name type_name = (Name) palloc0(NAMEDATALEN);
		Datum schema_datum;
		Datum type_name_datum;
		Datum inner_array_datum;

		tp = SearchSysCache1(TYPEOID, ObjectIdGetDatum(type_oid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for type %u", type_oid);

		typtup = (Form_pg_type) GETSTRUCT(tp);
		namestrcpy(type_name, NameStr(typtup->typname));
		schema_name = get_namespace_name(typtup->typnamespace);
		ReleaseSysCache(tp);

		type_name_datum = NameGetDatum(type_name);
		/* Using name in because creating from a char * (that may be null or too long). */
		schema_datum = DirectFunctionCall1(namein, CStringGetDatum(schema_name));

		accumArrayResult(schema_name_builder, schema_datum, false, NAMEOID, builder_context);
		accumArrayResult(schema_name_builder, type_name_datum, false, NAMEOID, builder_context);

		inner_array_datum = makeArrayResult(schema_name_builder, CurrentMemoryContext);

		accumArrayResultArr(outer_builder,
							inner_array_datum,
							false,
							name_array_type_oid,
							builder_context);
	}
	result = makeArrayResultArr(outer_builder, CurrentMemoryContext, false);

	MemoryContextDelete(builder_context);
	return result;
}

/*
 * Creates an aggref of the form:
 * finalize-agg(
 *                "sum(int)" TEXT,
 *                collation_schema_name NAME, collation_name NAME,
 *                input_types_array NAME[N][2],
 *                <partial-column-name> BYTEA,
 *                null::<return-type of sum(int)>
 *             )
 * here sum(int) is the input aggregate "inp" in the parameter-list.
 */
static Aggref *
get_finalize_aggref(Aggref *inp, Var *partial_state_var)
{
	Aggref *aggref;
	TargetEntry *te;
	char *aggregate_signature;
	Const *aggregate_signature_const, *collation_schema_const, *collation_name_const,
		*input_types_const, *return_type_const;
	Oid name_array_type_oid = get_array_type(NAMEOID);
	Var *partial_bytea_var;
	List *tlist = NIL;
	int tlist_attno = 1;
	List *argtypes = NIL;
	char *collation_name = NULL, *collation_schema_name = NULL;
	Datum collation_name_datum = (Datum) 0;
	Datum collation_schema_datum = (Datum) 0;
	Oid finalfnoid = get_finalizefnoid();

	argtypes = list_make5_oid(TEXTOID, NAMEOID, NAMEOID, name_array_type_oid, BYTEAOID);
	argtypes = lappend_oid(argtypes, inp->aggtype);

	aggref = makeNode(Aggref);
	aggref->aggfnoid = finalfnoid;
	aggref->aggtype = inp->aggtype;
	aggref->aggcollid = inp->aggcollid;
	aggref->inputcollid = inp->inputcollid;
	aggref->aggtranstype = InvalidOid; /* will be set by planner */
	aggref->aggargtypes = argtypes;
	aggref->aggdirectargs = NULL; /*relevant for hypothetical set aggs*/
	aggref->aggorder = NULL;
	aggref->aggdistinct = NULL;
	aggref->aggfilter = NULL;
	aggref->aggstar = false;
	aggref->aggvariadic = false;
	aggref->aggkind = AGGKIND_NORMAL;
	aggref->aggsplit = AGGSPLIT_SIMPLE;
	aggref->location = -1;
	/* Construct the arguments. */
	aggregate_signature = format_procedure_qualified(inp->aggfnoid);
	aggregate_signature_const = makeConst(TEXTOID,
										  -1,
										  DEFAULT_COLLATION_OID,
										  -1,
										  CStringGetTextDatum(aggregate_signature),
										  false,
										  false /* passbyval */
	);
	te = makeTargetEntry((Expr *) aggregate_signature_const, tlist_attno++, NULL, false);
	tlist = lappend(tlist, te);

	if (OidIsValid(inp->inputcollid))
	{
		/* Similar to generate_collation_name. */
		HeapTuple tp;
		Form_pg_collation colltup;
		tp = SearchSysCache1(COLLOID, ObjectIdGetDatum(inp->inputcollid));
		if (!HeapTupleIsValid(tp))
			elog(ERROR, "cache lookup failed for collation %u", inp->inputcollid);
		colltup = (Form_pg_collation) GETSTRUCT(tp);
		collation_name = pstrdup(NameStr(colltup->collname));
		collation_name_datum = DirectFunctionCall1(namein, CStringGetDatum(collation_name));

		collation_schema_name = get_namespace_name(colltup->collnamespace);
		if (collation_schema_name != NULL)
			collation_schema_datum =
				DirectFunctionCall1(namein, CStringGetDatum(collation_schema_name));
		ReleaseSysCache(tp);
	}
	collation_schema_const = makeConst(NAMEOID,
									   -1,
									   InvalidOid,
									   NAMEDATALEN,
									   collation_schema_datum,
									   (collation_schema_name == NULL) ? true : false,
									   false /* passbyval */
	);
	te = makeTargetEntry((Expr *) collation_schema_const, tlist_attno++, NULL, false);
	tlist = lappend(tlist, te);

	collation_name_const = makeConst(NAMEOID,
									 -1,
									 InvalidOid,
									 NAMEDATALEN,
									 collation_name_datum,
									 (collation_name == NULL) ? true : false,
									 false /* passbyval */
	);
	te = makeTargetEntry((Expr *) collation_name_const, tlist_attno++, NULL, false);
	tlist = lappend(tlist, te);

	input_types_const = makeConst(get_array_type(NAMEOID),
								  -1,
								  InvalidOid,
								  -1,
								  get_input_types_array_datum(inp),
								  false,
								  false /* passbyval */
	);
	te = makeTargetEntry((Expr *) input_types_const, tlist_attno++, NULL, false);
	tlist = lappend(tlist, te);

	partial_bytea_var = copyObject(partial_state_var);
	te = makeTargetEntry((Expr *) partial_bytea_var, tlist_attno++, NULL, false);
	tlist = lappend(tlist, te);

	return_type_const = makeNullConst(inp->aggtype, -1, inp->aggcollid);
	te = makeTargetEntry((Expr *) return_type_const, tlist_attno++, NULL, false);
	tlist = lappend(tlist, te);

	Assert(tlist_attno == 7);
	aggref->args = tlist;
	return aggref;
}

/*
 * Creates a partialize expr for the passed in agg:
 * partialize_agg(agg).
 */
static FuncExpr *
get_partialize_funcexpr(Aggref *agg)
{
	FuncExpr *partialize_fnexpr;
	Oid partfnoid, partargtype;
	partargtype = ANYELEMENTOID;
	partfnoid = LookupFuncName(list_make2(makeString(INTERNAL_SCHEMA_NAME), makeString(PARTIALFN)),
							   1,
							   &partargtype,
							   false);
	partialize_fnexpr = makeFuncExpr(partfnoid,
									 BYTEAOID,
									 list_make1(agg), /*args*/
									 InvalidOid,
									 InvalidOid,
									 COERCE_EXPLICIT_CALL);
	return partialize_fnexpr;
}

/*
 * Check if the supplied OID belongs to a valid bucket function
 * for continuous aggregates.
 */
static bool
function_allowed_in_cagg_definition(Oid funcid)
{
	FuncInfo *finfo = ts_func_cache_get_bucketing_func(funcid);
	if (finfo == NULL)
		return false;

	return finfo->allowed_in_cagg_definition;
}

/*
 * Initialize MatTableColumnInfo.
 */
static void
mattablecolumninfo_init(MatTableColumnInfo *matcolinfo, List *grouplist)
{
	matcolinfo->matcollist = NIL;
	matcolinfo->partial_seltlist = NIL;
	matcolinfo->partial_grouplist = grouplist;
	matcolinfo->mat_groupcolname_list = NIL;
	matcolinfo->matpartcolno = -1;
	matcolinfo->matpartcolname = NULL;
}

/*
 * Add Information required to create and populate the materialization table columns
 * a) create a columndef for the materialization table
 * b) create the corresponding expr to populate the column of the materialization table (e..g for a
 *    column that is an aggref, we create a partialize_agg expr to populate the column Returns: the
 *    Var corresponding to the newly created column of the materialization table
 *
 * Notes: make sure the materialization table columns do not save
 *        values computed by mutable function.
 *
 * Notes on TargetEntry fields:
 * - (resname != NULL) means it's projected in our case
 * - (ressortgroupref > 0) means part of GROUP BY, which can be projected or not, depending of the
 *                         value of the resjunk
 * - (resjunk == true) applies for GROUP BY columns that are not projected
 *
 */
static Var *
mattablecolumninfo_addentry(MatTableColumnInfo *out, Node *input, int original_query_resno,
							bool finalized, bool *skip_adding)
{
	int matcolno = list_length(out->matcollist) + 1;
	char colbuf[NAMEDATALEN];
	char *colname;
	TargetEntry *part_te = NULL;
	ColumnDef *col;
	Var *var;
	Oid coltype, colcollation;
	int32 coltypmod;

	*skip_adding = false;

	if (contain_mutable_functions(input))
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only immutable functions supported in continuous aggregate view"),
				 errhint("Make sure all functions in the continuous aggregate definition"
						 " have IMMUTABLE volatility. Note that functions or expressions"
						 " may be IMMUTABLE for one data type, but STABLE or VOLATILE for"
						 " another.")));
	}

	switch (nodeTag(input))
	{
		case T_Aggref:
		{
			FuncExpr *fexpr = get_partialize_funcexpr((Aggref *) input);
			PRINT_MATCOLNAME(colbuf, "agg", original_query_resno, matcolno);
			colname = colbuf;
			coltype = BYTEAOID;
			coltypmod = -1;
			colcollation = InvalidOid;
			col = makeColumnDef(colname, coltype, coltypmod, colcollation);
			part_te = makeTargetEntry((Expr *) fexpr, matcolno, pstrdup(colname), false);
		}
		break;

		case T_TargetEntry:
		{
			TargetEntry *tle = (TargetEntry *) input;
			bool timebkt_chk = false;

			if (IsA(tle->expr, FuncExpr))
				timebkt_chk = function_allowed_in_cagg_definition(((FuncExpr *) tle->expr)->funcid);

			if (tle->resname)
				colname = pstrdup(tle->resname);
			else
			{
				if (timebkt_chk)
					colname = DEFAULT_MATPARTCOLUMN_NAME;
				else
				{
					PRINT_MATCOLNAME(colbuf, "grp", original_query_resno, matcolno);
					colname = colbuf;

					/* For finalized form we skip adding extra group by columns. */
					*skip_adding = finalized;
				}
			}

			if (timebkt_chk)
			{
				tle->resname = pstrdup(colname);
				out->matpartcolno = matcolno;
				out->matpartcolname = pstrdup(colname);
			}
			else
			{
				/*
				 * Add indexes only for columns that are part of the GROUP BY clause
				 * and for finals form.
				 * We skip adding it because we'll not add the extra group by columns
				 * to the materialization hypertable anymore.
				 */
				if (!*skip_adding && tle->ressortgroupref > 0)
					out->mat_groupcolname_list =
						lappend(out->mat_groupcolname_list, pstrdup(colname));
			}

			coltype = exprType((Node *) tle->expr);
			coltypmod = exprTypmod((Node *) tle->expr);
			colcollation = exprCollation((Node *) tle->expr);
			col = makeColumnDef(colname, coltype, coltypmod, colcollation);
			part_te = (TargetEntry *) copyObject(input);

			/* Keep original resjunk if finalized or not time bucket. */
			if (!finalized || timebkt_chk)
			{
				/*
				 * Need to project all the partial entries so that
				 * materialization table is filled.
				 */
				part_te->resjunk = false;
			}

			part_te->resno = matcolno;

			if (timebkt_chk)
			{
				col->is_not_null = true;
			}

			if (part_te->resname == NULL)
			{
				part_te->resname = pstrdup(colname);
			}
		}
		break;

		case T_Var:
		{
			PRINT_MATCOLNAME(colbuf, "var", original_query_resno, matcolno);
			colname = colbuf;

			coltype = exprType(input);
			coltypmod = exprTypmod(input);
			colcollation = exprCollation(input);
			col = makeColumnDef(colname, coltype, coltypmod, colcollation);
			part_te = makeTargetEntry((Expr *) input, matcolno, pstrdup(colname), false);

			/* Need to project all the partial entries so that materialization table is filled. */
			part_te->resjunk = false;
			part_te->resno = matcolno;
		}
		break;

		default:
			elog(ERROR, "invalid node type %d", nodeTag(input));
			break;
	}
	Assert((!finalized && list_length(out->matcollist) == list_length(out->partial_seltlist)) ||
		   (finalized && list_length(out->matcollist) <= list_length(out->partial_seltlist)));
	Assert(col != NULL);
	Assert(part_te != NULL);

	if (!*skip_adding)
	{
		out->matcollist = lappend(out->matcollist, col);
	}

	out->partial_seltlist = lappend(out->partial_seltlist, part_te);

	var = makeVar(1, matcolno, coltype, coltypmod, colcollation, 0);
	return var;
}

/*
 * Add internal columns for the materialization table.
 */
static void
mattablecolumninfo_addinternal(MatTableColumnInfo *matcolinfo)
{
	Index maxRef;
	int colno = list_length(matcolinfo->partial_seltlist) + 1;
	ColumnDef *col;
	Var *chunkfn_arg1;
	FuncExpr *chunk_fnexpr;
	Oid chunkfnoid;
	Oid argtype[] = { OIDOID };
	Oid rettype = INT4OID;
	TargetEntry *chunk_te;
	Oid sortop, eqop;
	bool hashable;
	ListCell *lc;
	SortGroupClause *grpcl;

	/* Add a chunk_id column for materialization table */
	Node *vexpr = (Node *) makeVar(1, colno, INT4OID, -1, InvalidOid, 0);
	col = makeColumnDef(CONTINUOUS_AGG_CHUNK_ID_COL_NAME,
						exprType(vexpr),
						exprTypmod(vexpr),
						exprCollation(vexpr));
	matcolinfo->matcollist = lappend(matcolinfo->matcollist, col);

	/*
	 * Need to add an entry to the target list for computing chunk_id column
	 * : chunk_for_tuple( htid, table.*).
	 */
	chunkfnoid =
		LookupFuncName(list_make2(makeString(INTERNAL_SCHEMA_NAME), makeString(CHUNKIDFROMRELID)),
					   sizeof(argtype) / sizeof(argtype[0]),
					   argtype,
					   false);
	chunkfn_arg1 = makeVar(1, TableOidAttributeNumber, OIDOID, -1, 0, 0);

	chunk_fnexpr = makeFuncExpr(chunkfnoid,
								rettype,
								list_make1(chunkfn_arg1),
								InvalidOid,
								InvalidOid,
								COERCE_EXPLICIT_CALL);
	chunk_te = makeTargetEntry((Expr *) chunk_fnexpr,
							   colno,
							   pstrdup(CONTINUOUS_AGG_CHUNK_ID_COL_NAME),
							   false);
	matcolinfo->partial_seltlist = lappend(matcolinfo->partial_seltlist, chunk_te);
	/* Any internal column needs to be added to the group-by clause as well. */
	maxRef = 0;
	foreach (lc, matcolinfo->partial_seltlist)
	{
		Index ref = ((TargetEntry *) lfirst(lc))->ressortgroupref;

		if (ref > maxRef)
			maxRef = ref;
	}
	chunk_te->ressortgroupref =
		maxRef + 1; /* used by sortgroupclause to identify the targetentry */
	grpcl = makeNode(SortGroupClause);
	get_sort_group_operators(exprType((Node *) chunk_te->expr),
							 false,
							 true,
							 false,
							 &sortop,
							 &eqop,
							 NULL,
							 &hashable);
	grpcl->tleSortGroupRef = chunk_te->ressortgroupref;
	grpcl->eqop = eqop;
	grpcl->sortop = sortop;
	grpcl->nulls_first = false;
	grpcl->hashable = hashable;

	matcolinfo->partial_grouplist = lappend(matcolinfo->partial_grouplist, grpcl);
}

static Aggref *
add_partialize_column(Aggref *agg_to_partialize, AggPartCxt *cxt)
{
	Aggref *newagg;
	Var *var;
	bool skip_adding;

	/*
	 * Step 1: create partialize( aggref) column
	 * for materialization table.
	 */
	var = mattablecolumninfo_addentry(cxt->mattblinfo,
									  (Node *) agg_to_partialize,
									  cxt->original_query_resno,
									  false,
									  &skip_adding);
	cxt->added_aggref_col = true;
	/*
	 * Step 2: create finalize_agg expr using var
	 * for the column added to the materialization table.
	 */

	/* This is a var for the column we created. */
	newagg = get_finalize_aggref(agg_to_partialize, var);
	return newagg;
}

static void
set_var_mapping(Var *orig_var, Var *mapped_var, AggPartCxt *cxt)
{
	cxt->orig_vars = lappend(cxt->orig_vars, orig_var);
	cxt->mapped_vars = lappend(cxt->mapped_vars, mapped_var);
}

/*
 * Checks whether var has already been mapped and returns the
 * corresponding column of the materialization table.
 */
static Var *
var_already_mapped(Var *var, AggPartCxt *cxt)
{
	ListCell *lc_old, *lc_new;

	forboth (lc_old, cxt->orig_vars, lc_new, cxt->mapped_vars)
	{
		Var *orig_var = (Var *) lfirst_node(Var, lc_old);
		Var *mapped_var = (Var *) lfirst_node(Var, lc_new);

		/* There should be no subqueries so varlevelsup should not be a problem here. */
		if (var->varno == orig_var->varno && var->varattno == orig_var->varattno)
			return mapped_var;
	}
	return NULL;
}

static Node *
add_var_mutator(Node *node, AggPartCxt *cxt)
{
	if (node == NULL)
		return NULL;
	if (IsA(node, Aggref))
	{
		return node; /* don't process this further */
	}
	if (IsA(node, Var))
	{
		Var *orig_var, *mapped_var;
		bool skip_adding = false;

		mapped_var = var_already_mapped((Var *) node, cxt);
		/* Avoid duplicating columns in the materialization table. */
		if (mapped_var)
			/*
			 * There should be no subquery so mapped_var->varlevelsup
			 * should not be a problem here.
			 */
			return (Node *) copyObject(mapped_var);

		orig_var = (Var *) node;
		mapped_var = mattablecolumninfo_addentry(cxt->mattblinfo,
												 node,
												 cxt->original_query_resno,
												 false,
												 &skip_adding);
		set_var_mapping(orig_var, mapped_var, cxt);
		return (Node *) mapped_var;
	}
	return expression_tree_mutator(node, add_var_mutator, cxt);
}

static Node *
add_aggregate_partialize_mutator(Node *node, AggPartCxt *cxt)
{
	if (node == NULL)
		return NULL;
	/*
	 * Modify the aggref and create a partialize(aggref) expr
	 * for the materialization.
	 * Add a corresponding  columndef for the mat table.
	 * Replace the aggref with the ts_internal_cagg_final fn.
	 * using a Var for the corresponding column in the mat table.
	 * All new Vars have varno = 1 (for RTE 1).
	 */
	if (IsA(node, Aggref))
	{
		if (cxt->ignore_aggoid == ((Aggref *) node)->aggfnoid)
			return node; /* don't process this further */

		Aggref *newagg = add_partialize_column((Aggref *) node, cxt);
		return (Node *) newagg;
	}
	if (IsA(node, Var))
	{
		cxt->var_outside_of_aggref = true;
	}
	return expression_tree_mutator(node, add_aggregate_partialize_mutator, cxt);
}

typedef struct Cagg_havingcxt
{
	List *origq_tlist;
	List *finalizeq_tlist;
	AggPartCxt agg_cxt;
} cagg_havingcxt;

/*
 * This function modifies the passed in havingQual by mapping exprs to
 * columns in materialization table or finalized aggregate form.
 * Note that HAVING clause can contain only exprs from group-by or aggregates
 * and GROUP BY clauses cannot be aggregates.
 * (By the time we process havingQuals, all the group by exprs have been
 * processed and have associated columns in the materialization hypertable).
 * Example, if  the original query has
 * GROUP BY  colA + colB, colC
 *   HAVING colA + colB + sum(colD) > 10 OR count(colE) = 10
 *
 * The transformed havingqual would be
 * HAVING   matCol3 + finalize_agg( sum(matCol4) > 10
 *       OR finalize_agg( count(matCol5)) = 10
 *
 *
 * Note: GROUP BY exprs always appear in the query's targetlist.
 * Some of the aggregates from the havingQual  might also already appear in the targetlist.
 * We replace all existing entries with their corresponding entry from the modified targetlist.
 * If an aggregate (in the havingqual) does not exist in the TL, we create a
 *  materialization table column for it and use the finalize(column) form in the
 * transformed havingQual.
 */
static Node *
create_replace_having_qual_mutator(Node *node, cagg_havingcxt *cxt)
{
	if (node == NULL)
		return NULL;
	/*
	 * See if we already have a column in materialization hypertable for this
	 * expr. We do this by checking the existing targetlist
	 * entries for the query.
	 */
	ListCell *lc, *lc2;
	List *origtlist = cxt->origq_tlist;
	List *modtlist = cxt->finalizeq_tlist;
	forboth (lc, origtlist, lc2, modtlist)
	{
		TargetEntry *te = (TargetEntry *) lfirst(lc);
		TargetEntry *modte = (TargetEntry *) lfirst(lc2);
		if (equal(node, te->expr))
		{
			return (Node *) modte->expr;
		}
	}
	/*
	 * Didn't find a match in targetlist. If it is an aggregate,
	 * create a partialize column for it in materialization hypertable
	 * and return corresponding finalize expr.
	 */
	if (IsA(node, Aggref))
	{
		AggPartCxt *agg_cxt = &(cxt->agg_cxt);
		agg_cxt->added_aggref_col = false;
		Aggref *newagg = add_partialize_column((Aggref *) node, agg_cxt);
		Assert(agg_cxt->added_aggref_col == true);
		return (Node *) newagg;
	}
	return expression_tree_mutator(node, create_replace_having_qual_mutator, cxt);
}

static Node *
finalizequery_create_havingqual(FinalizeQueryInfo *inp, MatTableColumnInfo *mattblinfo)
{
	Query *orig_query = inp->final_userquery;
	if (orig_query->havingQual == NULL)
		return NULL;
	Node *havingQual = copyObject(orig_query->havingQual);
	Assert(inp->final_seltlist != NULL);
	cagg_havingcxt hcxt = { .origq_tlist = orig_query->targetList,
							.finalizeq_tlist = inp->final_seltlist,
							.agg_cxt.mattblinfo = mattblinfo,
							.agg_cxt.original_query_resno = 0,
							.agg_cxt.ignore_aggoid = get_finalizefnoid(),
							.agg_cxt.added_aggref_col = false,
							.agg_cxt.var_outside_of_aggref = false,
							.agg_cxt.orig_vars = NIL,
							.agg_cxt.mapped_vars = NIL };
	return create_replace_having_qual_mutator(havingQual, &hcxt);
}

/*
 * Init the finalize query data structure.
 * Parameters:
 * orig_query - the original query from user view that is being used as template for the finalize
 * query tlist_aliases - aliases for the view select list materialization table columns are created
 * . This will be returned in  the mattblinfo
 *
 * DO NOT modify orig_query. Make a copy if needed.
 * SIDE_EFFECT: the data structure in mattblinfo is modified as a side effect by adding new
 * materialize table columns and partialize exprs.
 */
static void
finalizequery_init(FinalizeQueryInfo *inp, Query *orig_query, MatTableColumnInfo *mattblinfo)
{
	AggPartCxt cxt;
	ListCell *lc;
	int resno = 1;

	inp->final_userquery = copyObject(orig_query);
	inp->final_seltlist = NIL;
	inp->final_havingqual = NULL;

	/* Set up the final_seltlist and final_havingqual entries */
	cxt.mattblinfo = mattblinfo;
	cxt.ignore_aggoid = InvalidOid;

	/* Set up the left over variable mapping lists */
	cxt.orig_vars = NIL;
	cxt.mapped_vars = NIL;

	/*
	 * We want all the entries in the targetlist (resjunk or not)
	 * in the materialization  table definition so we include group-by/having clause etc.
	 * We have to do 3 things here:
	 * 1) create a column for mat table
	 * 2) partialize_expr to populate it, and
	 * 3) modify the target entry to be a finalize_expr
	 *    that selects from the materialization table.
	 */
	foreach (lc, orig_query->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		TargetEntry *modte = copyObject(tle);
		cxt.added_aggref_col = false;
		cxt.var_outside_of_aggref = false;
		cxt.original_query_resno = resno;

		if (!inp->finalized)
		{
			/*
			 * If tle has aggrefs, get the corresponding
			 * finalize_agg expression and save it in modte.
			 * Also add correspong materialization table column info
			 * for the aggrefs in tle.
			 */
			modte = (TargetEntry *) expression_tree_mutator((Node *) modte,
															add_aggregate_partialize_mutator,
															&cxt);
		}

		/*
		 * We need columns for non-aggregate targets.
		 * If it is not a resjunk OR appears in the grouping clause.
		 */
		if (cxt.added_aggref_col == false && (tle->resjunk == false || tle->ressortgroupref > 0))
		{
			Var *var;
			bool skip_adding = false;
			var = mattablecolumninfo_addentry(cxt.mattblinfo,
											  (Node *) tle,
											  cxt.original_query_resno,
											  inp->finalized,
											  &skip_adding);

			/* Skip adding this column for finalized form. */
			if (skip_adding)
			{
				continue;
			}

			/* Fix the expression for the target entry. */
			modte->expr = (Expr *) var;
		}
		/* Check for left over variables (Var) of targets that contain Aggref. */
		if (cxt.added_aggref_col && cxt.var_outside_of_aggref && !inp->finalized)
		{
			modte = (TargetEntry *) expression_tree_mutator((Node *) modte, add_var_mutator, &cxt);
		}
		/*
		 * Construct the targetlist for the query on the
		 * materialization table. The TL maps 1-1 with the original query:
		 * e.g select a, min(b)+max(d) from foo group by a,timebucket(a);
		 * becomes
		 * select <a-col>,
		 * ts_internal_cagg_final(..b-col ) + ts_internal_cagg_final(..d-col)
		 * from mattbl
		 * group by a-col, timebucket(a-col)
		 */

		/*
		 * We copy the modte target entries, resnos should be the same for
		 * final_selquery and origquery. So tleSortGroupReffor the targetentry
		 * can be reused, only table info needs to be modified.
		 */
		Assert((!inp->finalized && modte->resno == resno) ||
			   (inp->finalized && modte->resno >= resno));
		resno++;
		if (IsA(modte->expr, Var))
		{
			modte->resorigcol = ((Var *) modte->expr)->varattno;
		}
		inp->final_seltlist = lappend(inp->final_seltlist, modte);
	}
	/*
	 * All grouping clause elements are in targetlist already.
	 * So let's check the having clause.
	 */
	if (!inp->finalized)
		inp->final_havingqual = finalizequery_create_havingqual(inp, mattblinfo);
}

/*
 * Create select query with the finalize aggregates
 * for the materialization table.
 * matcollist - column list for mat table
 * mattbladdress - materialization table ObjectAddress
 * This is the function responsible for creating the final
 * structures for selecting from the materialized hypertable
 * created for the Cagg which is
 * select * from _timescaldeb_internal._matrialized_hypertable_<xxx>
 */
static Query *
finalizequery_get_select_query(FinalizeQueryInfo *inp, List *matcollist,
							   ObjectAddress *mattbladdress, char *relname)
{
	Query *final_selquery = NULL;
	ListCell *lc;
	FromExpr *fromexpr;
	RangeTblEntry *rte;

	/*
	 * For initial cagg creation rtable will have only 1 entry,
	 * for alter table rtable will have multiple entries with our
	 * RangeTblEntry as last member.
	 * For cagg with joins, we need to create a new RTE and jointree
	 * which contains the information of the materialised hypertable
	 * that is created for this cagg.
	 */
	if (list_length(inp->final_userquery->jointree->fromlist) >= CONTINUOUS_AGG_MAX_JOIN_RELATIONS)
	{
		rte = makeNode(RangeTblEntry);
		rte->alias = makeAlias(relname, NIL);
		rte->inFromCl = true;
		rte->inh = true;
		rte->rellockmode = 1;
		rte->eref = copyObject(rte->alias);
	}
	else
		rte = llast_node(RangeTblEntry, inp->final_userquery->rtable);
	rte->relid = mattbladdress->objectId;
	rte->rtekind = RTE_RELATION;
	rte->relkind = RELKIND_RELATION;
	rte->tablesample = NULL;
	rte->eref->colnames = NIL;
	rte->selectedCols = NULL;
	/* Aliases for column names for the materialization table. */
	foreach (lc, matcollist)
	{
		ColumnDef *cdef = (ColumnDef *) lfirst(lc);
		rte->eref->colnames = lappend(rte->eref->colnames, makeString(cdef->colname));
		rte->selectedCols =
			bms_add_member(rte->selectedCols,
						   list_length(rte->eref->colnames) - FirstLowInvalidHeapAttributeNumber);
	}
	rte->requiredPerms |= ACL_SELECT;
	rte->insertedCols = NULL;
	rte->updatedCols = NULL;

	/* 2. Fixup targetlist with the correct rel information. */
	foreach (lc, inp->final_seltlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);
		if (IsA(tle->expr, Var))
		{
			tle->resorigtbl = rte->relid;
			tle->resorigcol = ((Var *) tle->expr)->varattno;
		}
	}

	CAGG_MAKEQUERY(final_selquery, inp->final_userquery);
	final_selquery->hasAggs = !inp->finalized;
	if (list_length(inp->final_userquery->jointree->fromlist) >= CONTINUOUS_AGG_MAX_JOIN_RELATIONS)
	{
		RangeTblRef *rtr;
		final_selquery->rtable = list_make1(rte);
		rtr = makeNode(RangeTblRef);
		rtr->rtindex = 1;
		fromexpr = makeFromExpr(list_make1(rtr), NULL);
	}
	else
	{
		final_selquery->rtable = inp->final_userquery->rtable;
		fromexpr = inp->final_userquery->jointree;
		fromexpr->quals = NULL;
	}

	/*
	 * Fixup from list. No quals on original table should be
	 * present here - they should be on the query that populates
	 * the mattable (partial_selquery). For the Cagg with join,
	 * we can not copy the fromlist from inp->final_userquery as
	 * it has two tables in this case.
	 */
	Assert(list_length(inp->final_userquery->jointree->fromlist) <=
		   CONTINUOUS_AGG_MAX_JOIN_RELATIONS);

	final_selquery->jointree = fromexpr;
	final_selquery->targetList = inp->final_seltlist;
	final_selquery->sortClause = inp->final_userquery->sortClause;

	if (!inp->finalized)
	{
		final_selquery->groupClause = inp->final_userquery->groupClause;
		/* Copy the having clause too */
		final_selquery->havingQual = inp->final_havingqual;
	}

	return final_selquery;
}

/*
 * Assign aliases to the targetlist in the query according to the
 * column_names provided in the CREATE VIEW statement.
 */
static void
fixup_userview_query_tlist(Query *userquery, List *tlist_aliases)
{
	if (tlist_aliases != NIL)
	{
		ListCell *lc;
		ListCell *alist_item = list_head(tlist_aliases);
		foreach (lc, userquery->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc);

			/* Junk columns don't get aliases. */
			if (tle->resjunk)
				continue;
			tle->resname = pstrdup(strVal(lfirst(alist_item)));
			alist_item = lnext_compat(tlist_aliases, alist_item);
			if (alist_item == NULL)
				break; /* done assigning aliases */
		}

		if (alist_item != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR), errmsg("too many column names specified")));
	}
}

/*
 * Modifies the passed in ViewStmt to do the following
 * a) Create a hypertable for the continuous agg materialization.
 * b) create a view that references the underlying
 * materialization table instead of the original table used in
 * the CREATE VIEW stmt.
 * Example:
 * CREATE VIEW mcagg ...
 * AS  select a, min(b)+max(d) from foo group by a,timebucket(a);
 *
 * Step 1. create a materialiation table which stores the partials for the
 * aggregates and the grouping columns + internal columns.
 * So we have a table like _materialization_hypertable
 * with columns:
 *( a, col1, col2, col3, internal-columns)
 * where col1 =  partialize(min(b)), col2= partialize(max(d)),
 * col3= timebucket(a))
 *
 * Step 2: Create a view with modified select query
 * CREATE VIEW mcagg
 * as
 * select a, finalize( col1) + finalize(col2))
 * from _materialization_hypertable
 * group by a, col3
 *
 * Step 3: Create a view to populate the materialization table
 * create view ts_internal_mcagg_view
 * as
 * select a, partialize(min(b)), partialize(max(d)), timebucket(a), <internal-columns>
 * from foo
 * group by <internal-columns> , a , timebucket(a);
 *
 * Notes: ViewStmt->query is the raw parse tree
 * panquery is the output of running parse_anlayze( ViewStmt->query)
 *               so don't recreate invalidation trigger.

 * Since 1.7, we support real time aggregation.
 * If real time aggregation is off i.e. materialized only, the mcagg vew is as described in Step 2.
 * If it is turned on
 * we build a union query that selects from the internal mat view and the raw hypertable
 *     (see build_union_query for details)
 * CREATE VIEW mcagg
 * as
 * SELECT * from
 *        ( SELECT a, finalize(col1) + finalize(col2) from ts_internal_mcagg_view
 *                 ---> query from Step 2 with additional where clause
 *          WHERE timecol < materialization threshold
 *          group by <internal-columns> , a , timebucket(a);
 *          UNION ALL
 *          SELECT a, min(b)+max(d) from foo ---> original view stmt
 *                              ----> with additional where clause
 *          WHERE timecol >= materialization threshold
 *          GROUP BY a, time_bucket(a)
 *        )
 */
static void
cagg_create(const CreateTableAsStmt *create_stmt, ViewStmt *stmt, Query *panquery,
			CAggTimebucketInfo *origquery_ht, WithClauseResult *with_clause_options)
{
	ObjectAddress mataddress;
	char relnamebuf[NAMEDATALEN];
	MatTableColumnInfo mattblinfo;
	FinalizeQueryInfo finalqinfo;
	CatalogSecurityContext sec_ctx;
	bool is_create_mattbl_index;

	Query *final_selquery;
	Query *partial_selquery;	/* query to populate the mattable*/
	Query *orig_userview_query; /* copy of the original user query for dummy view */
	Oid nspid;
	RangeVar *part_rel = NULL, *mat_rel = NULL, *dum_rel = NULL;
	int32 materialize_hypertable_id;
	bool materialized_only =
		DatumGetBool(with_clause_options[ContinuousViewOptionMaterializedOnly].parsed);
	bool finalized = DatumGetBool(with_clause_options[ContinuousViewOptionFinalized].parsed);

	finalqinfo.finalized = finalized;

	/*
	 * Assign the column_name aliases in CREATE VIEW to the query.
	 * No other modifications to panquery.
	 */
	fixup_userview_query_tlist(panquery, stmt->aliases);
	mattablecolumninfo_init(&mattblinfo, copyObject(panquery->groupClause));
	finalizequery_init(&finalqinfo, panquery, &mattblinfo);

	/*
	 * Invalidate all options on the stmt before using it
	 * The options are valid only for internal use (ts_continuous).
	 */
	stmt->options = NULL;

	/*
	 * Step 0: Add any internal columns needed for materialization based
	 *         on the user query's table.
	 */
	if (!finalized)
		mattablecolumninfo_addinternal(&mattblinfo);

	/* Step 1: create the materialization table. */
	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	materialize_hypertable_id = ts_catalog_table_next_seq_id(ts_catalog_get(), HYPERTABLE);
	ts_catalog_restore_user(&sec_ctx);
	PRINT_MATINTERNAL_NAME(relnamebuf, "_materialized_hypertable_%d", materialize_hypertable_id);
	mat_rel = makeRangeVar(pstrdup(INTERNAL_SCHEMA_NAME), pstrdup(relnamebuf), -1);
	is_create_mattbl_index =
		DatumGetBool(with_clause_options[ContinuousViewOptionCreateGroupIndex].parsed);
	mattablecolumninfo_create_materialization_table(&mattblinfo,
													materialize_hypertable_id,
													mat_rel,
													origquery_ht,
													is_create_mattbl_index,
													create_stmt->into->tableSpaceName,
													create_stmt->into->accessMethod,
													&mataddress);
	/*
	 * Step 2: Create view with select finalize from materialization table.
	 */
	final_selquery = finalizequery_get_select_query(&finalqinfo,
													mattblinfo.matcollist,
													&mataddress,
													mat_rel->relname);

	if (!materialized_only)
		final_selquery = build_union_query(origquery_ht,
										   mattblinfo.matpartcolno,
										   final_selquery,
										   panquery,
										   materialize_hypertable_id);

	/* Copy view acl to materialization hypertable. */
	ObjectAddress view_address = create_view_for_query(final_selquery, stmt->view);
	ts_copy_relation_acl(view_address.objectId, mataddress.objectId, GetUserId());

	/*
	 * Step 3: create the internal view with select partialize(..).
	 */
	partial_selquery =
		mattablecolumninfo_get_partial_select_query(&mattblinfo, panquery, finalqinfo.finalized);

	PRINT_MATINTERNAL_NAME(relnamebuf, "_partial_view_%d", materialize_hypertable_id);
	part_rel = makeRangeVar(pstrdup(INTERNAL_SCHEMA_NAME), pstrdup(relnamebuf), -1);
	create_view_for_query(partial_selquery, part_rel);

	/*
	 * Additional miscellaneous steps.
	 */

	/*
	 * Create a dummy view to store the user supplied view query.
	 * This is to get PG to display the view correctly without
	 * having to replicate the PG source code for make_viewdef.
	 */
	orig_userview_query = copyObject(panquery);
	PRINT_MATINTERNAL_NAME(relnamebuf, "_direct_view_%d", materialize_hypertable_id);
	dum_rel = makeRangeVar(pstrdup(INTERNAL_SCHEMA_NAME), pstrdup(relnamebuf), -1);
	create_view_for_query(orig_userview_query, dum_rel);
	/* Step 4: Add catalog table entry for the objects we just created. */
	nspid = RangeVarGetCreationNamespace(stmt->view);

	create_cagg_catalog_entry(materialize_hypertable_id,
							  origquery_ht->htid,
							  get_namespace_name(nspid), /*schema name for user view */
							  stmt->view->relname,
							  part_rel->schemaname,
							  part_rel->relname,
							  origquery_ht->bucket_width,
							  materialized_only,
							  dum_rel->schemaname,
							  dum_rel->relname,
							  finalized,
							  origquery_ht->parent_mat_hypertable_id);

	if (origquery_ht->bucket_width == BUCKET_WIDTH_VARIABLE)
	{
		const char *bucket_width;
		const char *origin = "";

		/*
		 * Variable-sized buckets work only with intervals.
		 */
		Assert(origquery_ht->interval != NULL);
		bucket_width = DatumGetCString(
			DirectFunctionCall1(interval_out, IntervalPGetDatum(origquery_ht->interval)));

		if (!TIMESTAMP_NOT_FINITE(origquery_ht->origin))
		{
			origin = DatumGetCString(
				DirectFunctionCall1(timestamp_out, TimestampGetDatum(origquery_ht->origin)));
		}

		/*
		 * These values are not used for anything except Assert's yet
		 * for the same reasons. Once the design of variable-sized buckets
		 * is finalized we will have a better idea of what schema is needed exactly.
		 * Until then the choice was made in favor of the most generic schema
		 * that can be optimized later.
		 */
		create_bucket_function_catalog_entry(materialize_hypertable_id,
											 get_func_namespace(
												 origquery_ht->bucket_func->funcid) !=
												 PG_PUBLIC_NAMESPACE,
											 get_func_name(origquery_ht->bucket_func->funcid),
											 bucket_width,
											 origin,
											 origquery_ht->timezone);
	}

	/* Step 5: Create trigger on raw hypertable -specified in the user view query. */
	cagg_add_trigger_hypertable(origquery_ht->htoid, origquery_ht->htid);
}

DDLResult
tsl_process_continuous_agg_viewstmt(Node *node, const char *query_string, void *pstmt,
									WithClauseResult *with_clause_options)
{
	const CreateTableAsStmt *stmt = castNode(CreateTableAsStmt, node);
	CAggTimebucketInfo timebucket_exprinfo;
	Oid nspid;
	bool finalized = with_clause_options[ContinuousViewOptionFinalized].parsed;
	ViewStmt viewstmt = {
		.type = T_ViewStmt,
		.view = stmt->into->rel,
		.query = stmt->into->viewQuery,
		.options = stmt->into->options,
		.aliases = stmt->into->colNames,
	};

	nspid = RangeVarGetCreationNamespace(stmt->into->rel);
	if (get_relname_relid(stmt->into->rel->relname, nspid))
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("continuous aggregate \"%s\" already exists, skipping",
							stmt->into->rel->relname)));
			return DDL_DONE;
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("continuous aggregate \"%s\" already exists", stmt->into->rel->relname),
					 errhint("Drop or rename the existing continuous aggregate"
							 " first or use another name.")));
		}
	}
	if (!with_clause_options[ContinuousViewOptionCompress].is_default)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot enable compression while creating a continuous aggregate"),
				 errhint("Use ALTER MATERIALIZED VIEW to enable compression.")));
	}

	timebucket_exprinfo = cagg_validate_query((Query *) stmt->into->viewQuery,
											  finalized,
											  get_namespace_name(nspid),
											  stmt->into->rel->relname);
	cagg_create(stmt, &viewstmt, (Query *) stmt->query, &timebucket_exprinfo, with_clause_options);

	if (!stmt->into->skipData)
	{
		Oid relid;
		ContinuousAgg *cagg;
		InternalTimeRange refresh_window = {
			.type = InvalidOid,
		};

		CommandCounterIncrement();

		/*
		 * We are creating a refresh window here in a similar way to how it's
		 * done in continuous_agg_refresh. We do not call the PG function
		 * directly since we want to be able to suppress the output in that
		 * function and adding a 'verbose' parameter to is not useful for a
		 * user.
		 */
		relid = get_relname_relid(stmt->into->rel->relname, nspid);
		cagg = ts_continuous_agg_find_by_relid(relid);
		Assert(cagg != NULL);
		refresh_window.type = cagg->partition_type;
		/*
		 * To determine inscribed/circumscribed refresh window for variable-sized
		 * buckets we should be able to calculate time_bucket(window.begin) and
		 * time_bucket(window.end). This, however, is not possible in general case.
		 * As an example, the minimum date is 4714-11-24 BC, which is before any
		 * reasonable default `origin` value. Thus for variable-sized buckets
		 * instead of minimum date we use -infinity since time_bucket(-infinity)
		 * is well-defined as -infinity.
		 *
		 * For more details see:
		 * - ts_compute_inscribed_bucketed_refresh_window_variable()
		 * - ts_compute_circumscribed_bucketed_refresh_window_variable()
		 */
		refresh_window.start = ts_continuous_agg_bucket_width_variable(cagg) ?
								   ts_time_get_nobegin(refresh_window.type) :
								   ts_time_get_min(refresh_window.type);
		refresh_window.end = ts_time_get_noend_or_max(refresh_window.type);

		continuous_agg_refresh_internal(cagg, &refresh_window, CAGG_REFRESH_CREATION, true, true);
	}
	return DDL_DONE;
}

/*
 * When a view is created (StoreViewQuery), 2 dummy rtable entries corresponding to "old" and
 * "new" are prepended to the rtable list. We remove these and adjust the varnos to recreate
 * the user or direct view query.
 */
static void
remove_old_and_new_rte_from_query(Query *query)
{
	List *rtable = query->rtable;
	Assert(list_length(rtable) >= 3);
	rtable = list_delete_first(rtable);
	query->rtable = list_delete_first(rtable);
	OffsetVarNodes((Node *) query, -2, 0);
	Assert(list_length(query->rtable) >= 1);
}

/*
 * Test the view definition of an existing continuous aggregate
 * for errors and attempt to rebuild it if required.
 */
static void
cagg_rebuild_view_definition(ContinuousAgg *agg, Hypertable *mat_ht)
{
	bool test_failed = false;
	char *relname = agg->data.user_view_name.data;
	char *schema = agg->data.user_view_schema.data;
	ListCell *lc1, *lc2;
	int sec_ctx;
	Oid uid, saved_uid;
	/* Cagg view created by the user. */
	Oid user_view_oid = relation_oid(agg->data.user_view_schema, agg->data.user_view_name);
	Relation user_view_rel = relation_open(user_view_oid, AccessShareLock);
	Query *user_query = get_view_query(user_view_rel);
	bool finalized = ContinuousAggIsFinalized(agg);

	/* Extract final query from user view query. */
	Query *final_query = copyObject(user_query);
	remove_old_and_new_rte_from_query(final_query);
	if (!agg->data.materialized_only)
	{
		final_query = destroy_union_query(final_query);
	}

	if (finalized)
	{
		/* This continuous aggregate does not have partials, do not check for defects. */
		relation_close(user_view_rel, NoLock);
		return;
	}

	FinalizeQueryInfo fqi;
	MatTableColumnInfo mattblinfo;
	ObjectAddress mataddress = {
		.classId = RelationRelationId,
		.objectId = mat_ht->main_table_relid,
	};

	Oid direct_view_oid = relation_oid(agg->data.direct_view_schema, agg->data.direct_view_name);
	Relation direct_view_rel = relation_open(direct_view_oid, AccessShareLock);
	Query *direct_query = copyObject(get_view_query(direct_view_rel));
	remove_old_and_new_rte_from_query(direct_query);
	CAggTimebucketInfo timebucket_exprinfo =
		cagg_validate_query(direct_query,
							finalized,
							NameStr(agg->data.user_view_schema),
							NameStr(agg->data.user_view_name));

	mattablecolumninfo_init(&mattblinfo, copyObject(direct_query->groupClause));
	fqi.finalized = finalized;
	finalizequery_init(&fqi, direct_query, &mattblinfo);

	mattablecolumninfo_addinternal(&mattblinfo);

	Query *view_query =
		finalizequery_get_select_query(&fqi, mattblinfo.matcollist, &mataddress, relname);

	if (!agg->data.materialized_only)
		view_query = build_union_query(&timebucket_exprinfo,
									   mattblinfo.matpartcolno,
									   view_query,
									   direct_query,
									   mat_ht->fd.id);

	if (list_length(mattblinfo.matcollist) != ts_get_relnatts(mat_ht->main_table_relid))
		/*
		 * There is a mismatch of columns between the current version's finalization view
		 * building logic and the existing schema of the materialization table. As of version
		 * 2.7.0 this only happens due to buggy view generation in previous versions. Do not
		 * rebuild those views since the materialization table can not be queried correctly.
		 */
		test_failed = true;

	/*
	 * When calling StoreViewQuery the target list names of the query have to
	 * match the view's tuple descriptor attribute names. But if a column of the continuous
	 * aggregate has been renamed, the query tree will not have the correct
	 * names in the target list, which will error out when calling
	 * StoreViewQuery. For that reason, we fetch the name from the user view
	 * relation and update the resource name in the query target list to match
	 * the name in the user view.
	 */
	TupleDesc desc = RelationGetDescr(user_view_rel);
	int i = 0;
	forboth (lc1, view_query->targetList, lc2, user_query->targetList)
	{
		TargetEntry *view_tle, *user_tle;
		FormData_pg_attribute *attr = TupleDescAttr(desc, i);
		view_tle = lfirst_node(TargetEntry, lc1);
		user_tle = lfirst_node(TargetEntry, lc2);
		if (view_tle->resjunk && user_tle->resjunk)
			break;
		else if (view_tle->resjunk || user_tle->resjunk)
		{
			/*
			 * This should never happen but if it ever does it's safer to
			 * error here instead of creating broken view definitions.
			 */
			test_failed = true;
			break;
		}
		view_tle->resname = user_tle->resname = NameStr(attr->attname);
		++i;
	}

	if (test_failed)
	{
		ereport(WARNING,
				(errmsg("Inconsistent view definitions for continuous aggregate view "
						"\"%s.%s\"",
						schema,
						relname),
				 errdetail("Continuous aggregate data possibly corrupted.\n"
						   "You may need to recreate the continuous aggregate with"
						   "CREATE MATERIALIZED VIEW.")));
	}
	else
	{
		SWITCH_TO_TS_USER(NameStr(agg->data.user_view_schema), uid, saved_uid, sec_ctx);
		StoreViewQuery(user_view_oid, view_query, true);
		CommandCounterIncrement();
		RESTORE_USER(uid, saved_uid, sec_ctx);
	}
	/*
	 * Keep locks until end of transaction and do not close the relation
	 * before the call to StoreViewQuery since it can otherwise release the
	 * memory for attr->attname, causing a segfault.
	 */
	relation_close(direct_view_rel, NoLock);
	relation_close(user_view_rel, NoLock);
}

Datum
tsl_cagg_try_repair(PG_FUNCTION_ARGS)
{
	Oid relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	char relkind = get_rel_relkind(relid);
	ContinuousAgg *cagg = NULL;

	if (RELKIND_VIEW == relkind)
		cagg = ts_continuous_agg_find_by_relid(relid);

	if (RELKIND_VIEW != relkind || !cagg)
	{
		ereport(WARNING,
				(errmsg("invalid OID \"%u\" for continuous aggregate view", relid),
				 errdetail("Check for database corruption.")));
		PG_RETURN_VOID();
	}

	Cache *hcache = ts_hypertable_cache_pin();

	Hypertable *mat_ht = ts_hypertable_cache_get_entry_by_id(hcache, cagg->data.mat_hypertable_id);
	Assert(mat_ht != NULL);

	cagg_rebuild_view_definition(cagg, mat_ht);

	ts_cache_release(hcache);

	PG_RETURN_VOID();
}

/*
 * Flip the view definition of an existing continuous aggregate from
 * real-time to materialized-only or vice versa depending on the current state.
 */
void
cagg_flip_realtime_view_definition(ContinuousAgg *agg, Hypertable *mat_ht)
{
	int sec_ctx;
	Oid uid, saved_uid;
	Query *result_view_query;

	/* User view query of the user defined CAGG. */
	Oid user_view_oid = relation_oid(agg->data.user_view_schema, agg->data.user_view_name);
	Relation user_view_rel = relation_open(user_view_oid, AccessShareLock);
	Query *user_query = copyObject(get_view_query(user_view_rel));
	/* Keep lock until end of transaction. */
	relation_close(user_view_rel, NoLock);
	remove_old_and_new_rte_from_query(user_query);

	/* Direct view query of the original user view definition at CAGG creation. */
	Oid direct_view_oid = relation_oid(agg->data.direct_view_schema, agg->data.direct_view_name);
	Relation direct_view_rel = relation_open(direct_view_oid, AccessShareLock);
	Query *direct_query = copyObject(get_view_query(direct_view_rel));
	/* Keep lock until end of transaction. */
	relation_close(direct_view_rel, NoLock);
	remove_old_and_new_rte_from_query(direct_query);

	CAggTimebucketInfo timebucket_exprinfo =
		cagg_validate_query(direct_query,
							agg->data.finalized,
							NameStr(agg->data.user_view_schema),
							NameStr(agg->data.user_view_name));

	/* Flip */
	agg->data.materialized_only = !agg->data.materialized_only;
	if (agg->data.materialized_only)
	{
		result_view_query = destroy_union_query(user_query);
	}
	else
	{
		/* Get primary partitioning column information of time bucketing. */
		const Dimension *mat_part_dimension = hyperspace_get_open_dimension(mat_ht->space, 0);
		result_view_query = build_union_query(&timebucket_exprinfo,
											  mat_part_dimension->column_attno,
											  user_query,
											  direct_query,
											  mat_ht->fd.id);
	}
	SWITCH_TO_TS_USER(NameStr(agg->data.user_view_schema), uid, saved_uid, sec_ctx);
	StoreViewQuery(user_view_oid, result_view_query, true);
	CommandCounterIncrement();
	RESTORE_USER(uid, saved_uid, sec_ctx);
}

void
cagg_rename_view_columns(ContinuousAgg *agg)
{
	ListCell *lc;
	int sec_ctx;
	Oid uid, saved_uid;

	/* User view query of the user defined CAGG. */
	Oid user_view_oid = relation_oid(agg->data.user_view_schema, agg->data.user_view_name);
	Relation user_view_rel = relation_open(user_view_oid, AccessShareLock);
	Query *user_query = copyObject(get_view_query(user_view_rel));
	remove_old_and_new_rte_from_query(user_query);

	/*
	 * When calling StoreViewQuery the target list names of the query have to
	 * match the view's tuple descriptor attribute names. But if a column of the
	 * continuous aggregate has been renamed, the query tree will not have the correct
	 * names in the target list, which will error out when calling StoreViewQuery.
	 * For that reason, we fetch the name from the user view relation and update the
	 * resource name in the query target list to match the name in the user view.
	 */
	TupleDesc desc = RelationGetDescr(user_view_rel);
	int i = 0;
	foreach (lc, user_query->targetList)
	{
		TargetEntry *user_tle;
		FormData_pg_attribute *attr = TupleDescAttr(desc, i);
		user_tle = lfirst_node(TargetEntry, lc);
		if (user_tle->resjunk)
			break;

		user_tle->resname = NameStr(attr->attname);
		++i;
	}

	SWITCH_TO_TS_USER(NameStr(agg->data.user_view_schema), uid, saved_uid, sec_ctx);
	StoreViewQuery(user_view_oid, user_query, true);
	CommandCounterIncrement();
	RESTORE_USER(uid, saved_uid, sec_ctx);

	/*
	 * Keep locks until end of transaction and do not close the relation
	 * before the call to StoreViewQuery since it can otherwise release the
	 * memory for attr->attname, causing a segfault.
	 */
	relation_close(user_view_rel, NoLock);
}

/*
 * Create Const of proper type for lower bound of watermark when
 * watermark has not been set yet.
 */
static Const *
cagg_boundary_make_lower_bound(Oid type)
{
	Datum value;
	int16 typlen;
	bool typbyval;

	get_typlenbyval(type, &typlen, &typbyval);
	value = ts_time_datum_get_nobegin_or_min(type);

	return makeConst(type, -1, InvalidOid, typlen, value, false, typbyval);
}

/*
 * Get oid of function to convert from our internal representation
 * to postgres representation.
 */
static Oid
cagg_get_boundary_converter_funcoid(Oid typoid)
{
	char *function_name;
	Oid argtyp[] = { INT8OID };

	switch (typoid)
	{
		case DATEOID:
			function_name = INTERNAL_TO_DATE_FUNCTION;
			break;
		case TIMESTAMPOID:
			function_name = INTERNAL_TO_TS_FUNCTION;
			break;
		case TIMESTAMPTZOID:
			function_name = INTERNAL_TO_TSTZ_FUNCTION;
			break;
		default:
			/*
			 * This should never be reached and unsupported datatypes
			 * should be caught at much earlier stages.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_TS_INTERNAL_ERROR),
					 errmsg("no converter function defined for datatype: %s",
							format_type_be(typoid))));
			pg_unreachable();
	}

	List *func_name = list_make2(makeString(INTERNAL_SCHEMA_NAME), makeString(function_name));
	Oid converter_oid = LookupFuncName(func_name, lengthof(argtyp), argtyp, false);

	Assert(OidIsValid(converter_oid));

	return converter_oid;
}

static FuncExpr *
build_conversion_call(Oid type, FuncExpr *boundary)
{
	/*
	 * If the partitioning column type is not integer we need to convert
	 * to proper representation.
	 */
	switch (type)
	{
		case INT2OID:
		case INT4OID:
		{
			/* Since the boundary function returns int8 we need to cast to proper type here. */
			Oid cast_oid = ts_get_cast_func(INT8OID, type);

			return makeFuncExpr(cast_oid,
								type,
								list_make1(boundary),
								InvalidOid,
								InvalidOid,
								COERCE_IMPLICIT_CAST);
		}
		case INT8OID:
			/* Nothing to do for int8. */
			return boundary;
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			/*
			 * date/timestamp/timestamptz need to be converted since
			 * we store them differently from postgres format.
			 */
			Oid converter_oid = cagg_get_boundary_converter_funcoid(type);
			return makeFuncExpr(converter_oid,
								type,
								list_make1(boundary),
								InvalidOid,
								InvalidOid,
								COERCE_EXPLICIT_CALL);
		}

		default:
			/*
			 * All valid types should be handled above, this should
			 * never be reached and error handling at earlier stages
			 * should catch this.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_TS_INTERNAL_ERROR),
					 errmsg("unsupported datatype for continuous aggregates: %s",
							format_type_be(type))));
			pg_unreachable();
	}
}

/*
 * Build function call that returns boundary for a hypertable
 * wrapped in type conversion calls when required.
 */
static FuncExpr *
build_boundary_call(int32 ht_id, Oid type)
{
	Oid argtyp[] = { INT4OID };
	FuncExpr *boundary;

	Oid boundary_func_oid =
		LookupFuncName(list_make2(makeString(INTERNAL_SCHEMA_NAME), makeString(BOUNDARY_FUNCTION)),
					   lengthof(argtyp),
					   argtyp,
					   false);
	List *func_args =
		list_make1(makeConst(INT4OID, -1, InvalidOid, 4, Int32GetDatum(ht_id), false, true));

	boundary = makeFuncExpr(boundary_func_oid,
							INT8OID,
							func_args,
							InvalidOid,
							InvalidOid,
							COERCE_EXPLICIT_CALL);

	return build_conversion_call(type, boundary);
}

static Node *
build_union_query_quals(int32 ht_id, Oid partcoltype, Oid opno, int varno, AttrNumber attno)
{
	Var *var = makeVar(varno, attno, partcoltype, -1, InvalidOid, InvalidOid);
	FuncExpr *boundary = build_boundary_call(ht_id, partcoltype);

	CoalesceExpr *coalesce = makeNode(CoalesceExpr);
	coalesce->coalescetype = partcoltype;
	coalesce->coalescecollid = InvalidOid;
	coalesce->args = list_make2(boundary, cagg_boundary_make_lower_bound(partcoltype));

	return (Node *) make_opclause(opno,
								  BOOLOID,
								  false,
								  (Expr *) var,
								  (Expr *) coalesce,
								  InvalidOid,
								  InvalidOid);
}

static RangeTblEntry *
make_subquery_rte(Query *subquery, const char *aliasname)
{
	RangeTblEntry *rte = makeNode(RangeTblEntry);
	ListCell *lc;

	rte->rtekind = RTE_SUBQUERY;
	rte->relid = InvalidOid;
	rte->subquery = subquery;
	rte->alias = makeAlias(aliasname, NIL);
	rte->eref = copyObject(rte->alias);

	foreach (lc, subquery->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc);
		if (!tle->resjunk)
			rte->eref->colnames = lappend(rte->eref->colnames, makeString(pstrdup(tle->resname)));
	}

	rte->lateral = false;
	rte->inh = false; /* never true for subqueries */
	rte->inFromCl = true;

	return rte;
}

/*
 * Build union query combining the materialized data with data from the raw data hypertable.
 *
 * q1 is the query on the materialization hypertable with the finalize call
 * q2 is the query on the raw hypertable which was supplied in the inital CREATE VIEW statement
 * returns a query as
 * SELECT * from (  SELECT * from q1 where <coale_qual>
 *                  UNION ALL
 *                  SELECT * from q2 where existing_qual and <coale_qual>
 * where coale_qual is: time < ----> (or >= )
 * COALESCE(_timescaledb_internal.to_timestamp(_timescaledb_internal.cagg_watermark( <htid>)),
 * '-infinity'::timestamp with time zone)
 * See build_union_quals for COALESCE clauses.
 */
static Query *
build_union_query(CAggTimebucketInfo *tbinfo, int matpartcolno, Query *q1, Query *q2,
				  int materialize_htid)
{
	ListCell *lc1, *lc2;
	List *col_types = NIL;
	List *col_typmods = NIL;
	List *col_collations = NIL;
	List *tlist = NIL;
	List *sortClause = NIL;
	int varno;
	Node *q2_quals = NULL;

	Assert(list_length(q1->targetList) <= list_length(q2->targetList));

	q1 = copyObject(q1);
	q2 = copyObject(q2);

	if (q1->sortClause)
		sortClause = copyObject(q1->sortClause);

	TypeCacheEntry *tce = lookup_type_cache(tbinfo->htpartcoltype, TYPECACHE_LT_OPR);

	varno = list_length(q1->rtable);
	q1->jointree->quals = build_union_query_quals(materialize_htid,
												  tbinfo->htpartcoltype,
												  tce->lt_opr,
												  varno,
												  matpartcolno);
	/*
	 * If there is join in CAgg definition then adjust varno
	 * to get time column from the hypertable in the join.
	 */
	if (list_length(q2->rtable) == CONTINUOUS_AGG_MAX_JOIN_RELATIONS)
	{
		RangeTblRef *rtref = linitial_node(RangeTblRef, q2->jointree->fromlist);
		RangeTblEntry *rte = list_nth(q2->rtable, rtref->rtindex - 1);
		RangeTblRef *rtref_other = lsecond_node(RangeTblRef, q2->jointree->fromlist);
		RangeTblEntry *rte_other = list_nth(q2->rtable, rtref_other->rtindex - 1);

		Oid normal_table_id = ts_is_hypertable(rte->relid) ? rte_other->relid : rte->relid;
		if (normal_table_id == rte->relid)
			varno = 2;
		else
			varno = 1;
	}
	else
		varno = list_length(q2->rtable);
	q2_quals = build_union_query_quals(materialize_htid,
									   tbinfo->htpartcoltype,
									   get_negator(tce->lt_opr),
									   varno,
									   tbinfo->htpartcolno);
	q2->jointree->quals = make_and_qual(q2->jointree->quals, q2_quals);

	Query *query = makeNode(Query);
	SetOperationStmt *setop = makeNode(SetOperationStmt);
	RangeTblEntry *rte_q1 = make_subquery_rte(q1, "*SELECT* 1");
	RangeTblEntry *rte_q2 = make_subquery_rte(q2, "*SELECT* 2");
	RangeTblRef *ref_q1 = makeNode(RangeTblRef);
	RangeTblRef *ref_q2 = makeNode(RangeTblRef);

	query->commandType = CMD_SELECT;
	query->rtable = list_make2(rte_q1, rte_q2);
	query->setOperations = (Node *) setop;

	setop->op = SETOP_UNION;
	setop->all = true;
	ref_q1->rtindex = 1;
	ref_q2->rtindex = 2;
	setop->larg = (Node *) ref_q1;
	setop->rarg = (Node *) ref_q2;

	forboth (lc1, q1->targetList, lc2, q2->targetList)
	{
		TargetEntry *tle = lfirst_node(TargetEntry, lc1);
		TargetEntry *tle2 = lfirst_node(TargetEntry, lc2);
		TargetEntry *tle_union;
		Var *expr;

		if (!tle->resjunk)
		{
			col_types = lappend_int(col_types, exprType((Node *) tle->expr));
			col_typmods = lappend_int(col_typmods, exprTypmod((Node *) tle->expr));
			col_collations = lappend_int(col_collations, exprCollation((Node *) tle->expr));

			expr = makeVarFromTargetEntry(1, tle);
			/*
			 * We need to use resname from q2 because that is the query from the
			 * initial CREATE VIEW statement so the VIEW can be updated in place.
			 */
			tle_union = makeTargetEntry((Expr *) copyObject(expr),
										list_length(tlist) + 1,
										tle2->resname,
										false);
			tle_union->resorigtbl = expr->varno;
			tle_union->resorigcol = expr->varattno;
			tle_union->ressortgroupref = tle->ressortgroupref;

			tlist = lappend(tlist, tle_union);
		}
	}

	query->targetList = tlist;

	if (sortClause)
	{
		query->sortClause = sortClause;
		query->jointree = makeFromExpr(NIL, NULL);
	}

	setop->colTypes = col_types;
	setop->colTypmods = col_typmods;
	setop->colCollations = col_collations;

	return query;
}

/*
 * Extract the final view from the UNION ALL query.
 *
 * q1 is the query on the materialization hypertable with the finalize call
 * q2 is the query on the raw hypertable which was supplied in the inital CREATE VIEW statement
 * returns q1 from:
 * SELECT * from (  SELECT * from q1 where <coale_qual>
 *                  UNION ALL
 *                  SELECT * from q2 where existing_qual and <coale_qual>
 * where coale_qual is: time < ----> (or >= )
 * COALESCE(_timescaledb_internal.to_timestamp(_timescaledb_internal.cagg_watermark( <htid>)),
 * '-infinity'::timestamp with time zone)
 * The WHERE clause of the final view is removed.
 */
static Query *
destroy_union_query(Query *q)
{
	Assert(q->commandType == CMD_SELECT &&
		   ((SetOperationStmt *) q->setOperations)->op == SETOP_UNION &&
		   ((SetOperationStmt *) q->setOperations)->all == true);

	/* Get RTE of the left-hand side of UNION ALL. */
	RangeTblEntry *rte = linitial(q->rtable);
	Assert(rte->rtekind == RTE_SUBQUERY);

	Query *query = copyObject(rte->subquery);

	/* Delete the WHERE clause from the final view. */
	query->jointree->quals = NULL;

	return query;
}

/*
 * Return Oid for a schema-qualified relation.
 */
static Oid
relation_oid(NameData schema, NameData name)
{
	return get_relname_relid(NameStr(name), get_namespace_oid(NameStr(schema), false));
}
