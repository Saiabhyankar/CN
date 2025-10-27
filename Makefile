CXX = g++
CXXFLAGS = -std=c++11 -Wall -O2

SRC_DIR = test
DST_DIR = check
TEST_FILE = $(SRC_DIR)/sample.bin
RECEIVER_PORT = 8888
RECORD_SIZE = 512
BLAST_SIZE = 1000
LOSS_PERCENT = 2

all: sender receiver

sender: sender.cpp udp_file_transfer.h
	$(CXX) $(CXXFLAGS) sender.cpp -o sender

receiver: receiver.cpp udp_file_transfer.h
	$(CXX) $(CXXFLAGS) receiver.cpp -o receiver

clean:
	rm -f sender receiver
	rm -f $(DST_DIR)/*.received
	rm -f receiver.log

# Run the actual transfer using your existing test file
test: all
	@echo "=== File Info ==="
	@stat -c "File: %n | Size: %s bytes" $(TEST_FILE)
	@echo "\n=== Starting receiver (loss = $(LOSS_PERCENT)%) ==="
	mkdir -p $(DST_DIR)
	./receiver $(RECEIVER_PORT) $(DST_DIR) $(LOSS_PERCENT) > receiver.log 2>&1 &
	@sleep 2
	@echo "\n=== Starting sender ==="
	./sender 127.0.0.1 $(RECEIVER_PORT) $(TEST_FILE) $(RECORD_SIZE) $(BLAST_SIZE)
	@sleep 1
	@echo "\n=== Transfer complete ==="
	@pkill receiver 2>/dev/null || true
	@echo "Receiver log available at receiver.log"

.PHONY: all clean test
