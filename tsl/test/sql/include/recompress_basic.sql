-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

CREATE OR REPLACE VIEW compressed_chunk_info_view AS
SELECT
   h.schema_name AS hypertable_schema,
   h.table_name AS hypertable_name,
   c.schema_name as chunk_schema,
   c.table_name as chunk_name,
   c.status as chunk_status, 
   comp.schema_name as compressed_chunk_schema,
   comp.table_name as compressed_chunk_name
FROM
   _timescaledb_catalog.hypertable h JOIN
  _timescaledb_catalog.chunk c ON h.id = c.hypertable_id
   LEFT JOIN _timescaledb_catalog.chunk comp
ON comp.id = c.compressed_chunk_id
;

CREATE TABLE test2 (timec timestamptz NOT NULL, i integer ,
      b bigint, t text);
SELECT table_name from create_hypertable('test2', 'timec', chunk_time_interval=> INTERVAL '7 days');

INSERT INTO test2 SELECT q, 10, 11, 'hello' FROM generate_series( '2020-01-03 10:00:00+00', '2020-01-03 12:00:00+00' , '5 min'::interval) q;
ALTER TABLE test2 set (timescaledb.compress, 
timescaledb.compress_segmentby = 'b', 
timescaledb.compress_orderby = 'timec DESC');

SELECT compress_chunk(c)
FROM show_chunks('test2') c;

---insert into the middle of the range ---
INSERT INTO test2 values ( '2020-01-03 10:01:00+00', 20, 11, '2row'); 
INSERT INTO test2 values ( '2020-01-03 11:01:00+00', 20, 11, '3row'); 
INSERT INTO test2 values ( '2020-01-03 12:01:00+00', 20, 11, '4row'); 
--- insert a new segment  by ---
INSERT INTO test2 values ( '2020-01-03 11:01:00+00', 20, 12, '12row'); 

SELECT time_bucket(INTERVAL '2 hour', timec), b, count(*)
FROM test2
GROUP BY time_bucket(INTERVAL '2 hour', timec), b
ORDER BY 1, 2;
 
--check status for chunk --
SELECT chunk_status,
       chunk_name as "CHUNK_NAME"
FROM compressed_chunk_info_view 
WHERE hypertable_name = 'test2' ORDER BY chunk_name;

SELECT compressed_chunk_schema || '.' || compressed_chunk_name as "COMP_CHUNK_NAME",
        chunk_schema || '.' || chunk_name as "CHUNK_NAME"
FROM compressed_chunk_info_view 
WHERE hypertable_name = 'test2' \gset

SELECT count(*) from test2;

SELECT recompress_chunk(:'CHUNK_NAME'::regclass); 

SELECT chunk_status,
       chunk_name as "CHUNK_NAME"
FROM compressed_chunk_info_view 
WHERE hypertable_name = 'test2' ORDER BY chunk_name;

--- insert into a compressed chunk again + a new chunk--
INSERT INTO test2 values ( '2020-01-03 11:01:03+00', 20, 11, '33row'), 
                         ( '2020-01-03 11:01:06+00', 20, 11, '36row'), 
                         ( '2020-01-03 11:02:00+00', 20, 12, '12row'), 
                         ( '2020-04-03 00:02:00+00', 30, 13, '3013row'); 

SELECT time_bucket(INTERVAL '2 hour', timec), b, count(*)
FROM test2
GROUP BY time_bucket(INTERVAL '2 hour', timec), b
ORDER BY 1, 2;

--chunk status should be unordered for the previously compressed chunk
SELECT chunk_status,
       chunk_name as "CHUNK_NAME"
FROM compressed_chunk_info_view 
WHERE hypertable_name = 'test2' ORDER BY chunk_name;

SELECT add_compression_policy AS job_id
  FROM add_compression_policy('test2', '30d'::interval) \gset
CALL run_job(:job_id);
CALL run_job(:job_id);

-- status should be compressed ---
SELECT chunk_status,
       chunk_name as "CHUNK_NAME"
FROM compressed_chunk_info_view 
WHERE hypertable_name = 'test2' ORDER BY chunk_name;

\set ON_ERROR_STOP 0
-- call recompress_chunk when status is not unordered
SELECT recompress_chunk(:'CHUNK_NAME'::regclass, true); 
SELECT recompress_chunk(:'CHUNK_NAME'::regclass, false); 

--now decompress it , then try and recompress 
SELECT decompress_chunk(:'CHUNK_NAME'::regclass);
SELECT recompress_chunk(:'CHUNK_NAME'::regclass);
\set ON_ERROR_STOP 1