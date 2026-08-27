#pragma once

#define RINGBUFFER_MIRROE_FAIL -1
#define RINGBUFFER_EMPTY 0
#define RINGBUFFER_FULL 1
#define RINGBUFFER_NORMAL 2

#define RINGBUFFER_SAFE_FAIL -1

#define RINGBUFFER_READ_FAIL -1

#define RINGBUFFER_WRITE_FAIL -1
#define RINGBUFFER_WRITE_EMPTY 0

#define RINGBUFFER_DESTROY_FAIL -1
#define RINGBUFFER_DESTROY_SUCCESSFUL 0

#define Ringbuffer_CREATE_FAIL -1
//写代码需要有封装思想和调用思想，因此在实现某一个具体文件的时候，大多数应该采用集合起来

class Ringbuffer 
{
public:
    Ringbuffer(unsigned size);
    ~Ringbuffer();

    Ringbuffer(const Ringbuffer &) = delete;
    Ringbuffer &operator=(const Ringbuffer &) = delete;

    int write(const char *data,unsigned int length);
    int read(char *data,unsigned int length);
    int free_space();
    int data_space();

private:

    int Ringbuffer_mirror_inspect();

    //环形缓冲区的大小
    unsigned int size;
    //环形缓冲区的起始地址
    char* buffer;
    //环形缓冲区的读指针
    unsigned int read_pos; //这里初看不是指针的原因其实是在实际使用中，我们采用了数组的形式来实现环形缓冲区，因此这里的读指针和写指针都是数组的下标
    //环形缓冲区的写指针
    unsigned int write_pos;
    //环形缓冲区的满标志
    bool full;
    unsigned int read_pos_mirror;
    //环形缓冲区的写指针镜像位
    unsigned int write_pos_mirror;
    //环形缓冲区的读指针镜像位
};

/*
struct Ringbuffer
{
    //环形缓冲区的大小
    unsigned int size;
    //环形缓冲区的起始地址
    char* buffer;
    //环形缓冲区的读指针
    unsigned int read_pos; //这里初看不是指针的原因其实是在实际使用中，我们采用了数组的形式来实现环形缓冲区，因此这里的读指针和写指针都是数组的下标
    //环形缓冲区的写指针
    unsigned int write_pos;
    //环形缓冲区的满标志
    bool full;

    unsigned int read_pos_mirror;
    //环形缓冲区的写指针镜像位
    unsigned int write_pos_mirror;
    //环形缓冲区的读指针镜像位
};

void Ringbuffer_init(Ringbuffer *rb, unsigned int size);
int Ringbuffer_write(Ringbuffer *rb, const char *data,unsigned int length);
int Ringbuffer_mirror_inspect(Ringbuffer *rb);
int Ringbuffer_read(Ringbuffer *rb, char *data,unsigned int length);
int Ringbuffer_data_destroy(Ringbuffer *rb);
int Ringbuffer_write_data_length_get(Ringbuffer *rb);
int Ringbuffer_read_data_length_get(Ringbuffer *rb);
*/