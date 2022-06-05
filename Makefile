
TARGET = ssd_fuse
TEST   = ssd_fuse_dut

$(TARGET): ssd_fuse.c $(TEST)
	gcc -Wall ssd_fuse.c `pkg-config fuse3 --cflags --libs` -D_FILE_OFFSET_BITS=64 -o ssd_fuse
$(TEST): ssd_fuse_dut.c
	gcc -Wall ssd_fuse_dut.c -o ssd_fuse_dut

clean:
	rm $(TARGET) $(TEST)