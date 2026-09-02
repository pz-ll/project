# 编译器
CC = gcc
# 编译参数：Wall开启所有警告，g包含gdb调试信息
CFLAGS = -Wall -g
# 所有目标文件列表（新增 log.o）
OBJS = main.o http.o file.o log.o
# 最终生成的可执行程序名
TARGET = mini_httpd

# 默认目标：编译全部
all: $(TARGET)

# 链接所有目标文件，生成可执行程序
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# 每个.c文件的编译规则
# 头文件改动会触发对应文件重新编译
main.o: main.c http.h
	$(CC) $(CFLAGS) -c main.c

http.o: http.c http.h
	$(CC) $(CFLAGS) -c http.c

file.o: file.c http.h
	$(CC) $(CFLAGS) -c file.c

# 新增：日志模块编译
log.o: log.c http.h
	$(CC) $(CFLAGS) -c log.c

# 清理：删除所有目标文件、可执行程序、日志文件
clean:
	rm -rf *.o $(TARGET) log.txt