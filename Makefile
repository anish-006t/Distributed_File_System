CC=gcc
CFLAGS=-Wall -Wextra -O2 -g -pthread

SRC_DIR=src
BUILD_DIR=build
BIN_DIR=bin

COMMON_SRCS= \
	$(SRC_DIR)/common/socket_utils.c \
	$(SRC_DIR)/common/strutils.c \
	$(SRC_DIR)/common/log.c \
	$(SRC_DIR)/common/file_utils.c \
	$(SRC_DIR)/common/protocol.c \
	$(SRC_DIR)/common/hashmap.c

NM_SRCS= \
	$(SRC_DIR)/nm/main.c \
	$(SRC_DIR)/nm/nm_state.c \
	$(SRC_DIR)/nm/exec_utils.c

SS_SRCS= \
	$(SRC_DIR)/ss/main.c \
	$(SRC_DIR)/ss/ss_state.c \
	$(SRC_DIR)/ss/sentence.c

CLIENT_SRCS= \
	$(SRC_DIR)/client/main.c \
	$(SRC_DIR)/client/client_ui.c

COMMON_OBJS=$(COMMON_SRCS:%.c=$(BUILD_DIR)/%.o)
NM_OBJS=$(NM_SRCS:%.c=$(BUILD_DIR)/%.o)
SS_OBJS=$(SS_SRCS:%.c=$(BUILD_DIR)/%.o)
CLIENT_OBJS=$(CLIENT_SRCS:%.c=$(BUILD_DIR)/%.o)
TEST_SRCS= \
	$(SRC_DIR)/tests/test_driver.c
TEST_OBJS=$(TEST_SRCS:%.c=$(BUILD_DIR)/%.o)

TARGET_NM=$(BIN_DIR)/nm
TARGET_SS=$(BIN_DIR)/ss
TARGET_CLIENT=$(BIN_DIR)/client
TARGET_TEST=$(BIN_DIR)/test_driver

.PHONY: all clean dirs

all: dirs $(TARGET_NM) $(TARGET_SS) $(TARGET_CLIENT) $(TARGET_TEST)

$(TARGET_NM): $(COMMON_OBJS) $(NM_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_SS): $(COMMON_OBJS) $(SS_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_CLIENT): $(COMMON_OBJS) $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET_TEST): $(COMMON_OBJS) $(TEST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -c $< -o $@

dirs:
	@mkdir -p $(BUILD_DIR) $(BIN_DIR)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)
