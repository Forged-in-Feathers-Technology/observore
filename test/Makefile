# Host build of the radio-independent logic.  No ESP-IDF required.
CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O1 -g -DOBSERVORE_HOST_TEST
INCS     = -I../main
SRCS     = test_detect.c ../main/observore_detect.c ../main/observore_track.c \
           ../main/observore_mute.c ../main/observore_util.c \
           ../main/observore_notify_fmt.c ../main/observore_wps.c \
           ../main/observore_version.c ../main/observore_heapwatch.c \
           ../main/observore_surroundings.c ../main/observore_battery.c ../main/observore_rtc.c ../main/observore_watch.c \
           ../main/observore_touchcal.c \
           ../main/observore_census.c ../main/observore_odid.c ../main/observore_monitors.c ../main/observore_tap.c

# Headers count as sources here.  Most of what this suite pins down lives in
# them -- class weights, the verdict thresholds, the follower ceiling -- and
# without this line a header-only change leaves the old binary in place and
# `make && ./run` reports on code that is no longer there.  That has produced
# a false pass in this repository before.
HDRS     = $(wildcard ../main/*.h)

run: $(SRCS) $(HDRS) ../main/observore_oui_table.h
	$(CC) $(CFLAGS) $(INCS) $(SRCS) -o run

../main/observore_oui_table.h:
	cd .. && python3 tools/gen_oui_table.py

.PHONY: test clean
test: run
	./run

clean:
	rm -f run
