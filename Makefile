# immotus_appender — append-only table access method for PostgreSQL
#
# Build:  make install PG_CONFIG=/path/to/pg_config
# Clean:  make clean

MODULE_big = immotus_appender

OBJS = \
        src/appender.o \
        src/appender_datafile.o \
        src/appender_lifecycle.o \
        src/appender_memtuple.o \
        src/appender_metadata.o \
        src/appender_writer.o \
        src/appender_xlog.o
PGFILEDESC = "immotus_appender — append-only table access method for PostgreSQL"

EXTENSION = immotus_appender
DATA = sql/immotus_appender--1.0.sql

REGRESS = appender toast
ISOLATION = concurrency
TAP_TESTS = 1

# Standalone extension — use PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
