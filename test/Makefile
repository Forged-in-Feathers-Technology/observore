# Host build of the radio-independent logic.  No ESP-IDF required.
CC      ?= cc
# The device table is per board on the device, so the suite names its own
# size rather than inheriting whichever board was configured last -- a
# test whose meaning changes with an unrelated board profile is not a test.
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -DOBSERVORE_HOST_TEST \
           -DOBSERVORE_MAX_DEVICES=192
INCS     = -I../main
SRCS     = test_detect.c ../main/observore_detect.c ../main/observore_track.c \
           ../main/observore_mute.c ../main/observore_util.c \
           ../main/observore_notify_fmt.c ../main/observore_wps.c \
           ../main/observore_version.c ../main/observore_heapwatch.c \
           ../main/observore_surroundings.c ../main/observore_battery.c ../main/observore_rtc.c ../main/observore_watch.c \
           ../main/observore_touchcal.c ../main/observore_clock.c \
           ../main/observore_census.c ../main/observore_census_wire.c ../main/observore_odid.c ../main/observore_peer.c ../main/observore_monitors.c ../main/observore_tap.c \
           ../main/observore_httpscert.c \
           ../main/observore_watchcfg.c \
           ../main/observore_tz.c \
           ../main/observore_gps.c \
           ../main/observore_ultrasound.c \
           ../main/observore_csv.c

# Headers count as sources here.  Most of what this suite pins down lives in
# them -- class weights, the verdict thresholds, the follower ceiling -- and
# without this line a header-only change leaves the old binary in place and
# `make && ./run` reports on code that is no longer there.  That has produced
# a false pass in this repository before.
HDRS     = $(wildcard ../main/*.h)

# The census exchange is compiled in for `run`, at the smallest board's peer
# table so that eviction is cheap to reach, and compiled out for
# `run-nocensus`. Every default build is that second one, and the census
# tests that were written for a node on its own must hold in both.
CENSUS_ON = -DCONFIG_OBSERVORE_MESH_CENSUS=1 -DCONFIG_OBSERVORE_PEER_CENSUS_MAX=21

run: $(SRCS) $(HDRS) ../main/observore_oui_table.h
	$(CC) $(CFLAGS) $(CENSUS_ON) $(INCS) $(SRCS) -o run -lm

run-nocensus: $(SRCS) $(HDRS) ../main/observore_oui_table.h
	$(CC) $(CFLAGS) $(INCS) $(SRCS) -o run-nocensus -lm

../main/observore_oui_table.h:
	cd .. && python3 tools/gen_oui_table.py

.PHONY: test clean
test: run run-nocensus
	./run
	./run-nocensus

clean:
	rm -f run run-nocensus
