-- SQLite Remote Query - Demo
-- Run: sqlite3 < example/demo.sql

.load ./build/remote

-- Connect all remote tables in one call
SELECT remote_connect_db('amd@RSN-SWSLAB-03-L', '/home/amd/rocpd-7390.db', 'amd123');

-- List available tables
.tables

.headers on
.mode column

SELECT * FROM rocpd_kernel_dispatch_eb95343b77a98cb86f9e54335edbb3f4;

SELECT kd.id, kd.nid, kd.pid, ks.kernel_name FROM rocpd_kernel_dispatch_eb95343b77a98cb86f9e54335edbb3f4 as kd INNER JOIN rocpd_info_kernel_symbol_eb95343b77a98cb86f9e54335edbb3f4 as ks ON kd.kernel_id = ks.id;
