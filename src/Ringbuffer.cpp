/*
    本文件主要用于实现环形缓冲区的核心逻辑
    在缓冲区满时再写数据，会有两种处理方式，第一种是丢数据，第二钟抛出异常
    在缓冲区空时读数据，一定要有一个参照数据，如果是空的我们就返回错误，数据可以正常使用（保持FIFO特性）
    关于怎么判断缓冲区是空还是满的
    核心是使用一个线性地址空间做循环读写
*/

#include "Ringbuffer.hpp"
#include <cstring>
#include <iostream>

//初始化
Ringbuffer::Ringbuffer(unsigned int size)
{
    if(size == 0)
    {
        printf("Can't create a Ringbuffer that zero size");
    }

    this->size = size;
    this->buffer = new char[size];
    this->write_pos = 0;
    this->read_pos = 0;
    this->full = 0;
    this->read_pos_mirror = 0;
    this->write_pos_mirror = 0;
}
//初始化的过程其实是比较简单的，主要是分配内存空间和初始化结构体成员变量的值
//这样把一个模块拆成初始化+内部逻辑实现+数据处理+安全措施，其实可以让流程更简单很多

//析构函数：释放内存
Ringbuffer::~Ringbuffer()
{
    if(this->buffer != nullptr)
    {
        delete [] this->buffer;
        this->buffer = nullptr;
    }
}

//实现
/*
    这里主要是一个只有环形缓冲区的实现，我们可能要判断读写指针的高低位逻辑，以及判断缓存区是空，满，半空半满等等数据的逻辑
*/

/*
    核心是设计一个数据结构实现数组环形缓冲区的读写指针的镜像位是否正确，主要是为了保证环形缓冲区的读写逻辑正确性。
    我们思考一下，我们怎么使用镜像位来进行操作，现在有一个环形缓冲区，缓冲区大小为N，读指针与写指针都是从0开始的，
    当写指针到达N-1的时候，还可以继续写入数据，那么写指针就会回到0，这个点没有办法确定此时读指针与写指针相等的时候是空的还是满的，
    说明镜像位其实相当简单，我们是把镜像位看成标志位，当写指针达到数据的缓冲区的长度的限制的时候，我们就把这个镜像位反转
    与此同时判断实际读写指针的值与镜像标志位的值，用来判断是否为满
*/
int Ringbuffer::Ringbuffer_mirror_inspect()
{
    //安全检查
    if(this->buffer == nullptr)
    {
        return RINGBUFFER_MIRROE_FAIL;
    }

    //现在就是同时判断write_poss,write_poss_mirror,read_poss,read_poss_mirror这四个量来判断是否为满
    if(this->read_pos == this->write_pos && this->read_pos_mirror == this->write_pos_mirror)
    {
        this->full = 0;
        return RINGBUFFER_EMPTY;
    }
    if(this->read_pos == this->write_pos && this->read_pos_mirror != this->write_pos_mirror)
    {
        this->full = 1;
        return RINGBUFFER_FULL;
    }

    this->full = 0;
    return RINGBUFFER_NORMAL;
}

int Ringbuffer::write( const char *data,unsigned int length)
{
    //安全检查
    if(data == nullptr || length == 0)
    {
        return RINGBUFFER_SAFE_FAIL;
    }

    //在写入的时候，我们需要实现判断是否具有足够的套件来写入，前面有一个是实现用镜像位实现
    int Ringbuffer_data_state = this->Ringbuffer_mirror_inspect();
    if(Ringbuffer_data_state == RINGBUFFER_FULL)
    {
        return RINGBUFFER_WRITE_FAIL;
    }

    //如果具备了足够多的空间，我们可以进行数据写入
    //第一步拷贝数据，第二步移动指针的数据，然后再拷贝
    //在拷贝之前，需要有empty_data_length，需要计算出还剩多少，然后在我们需要有empty_data_length = 0的时候，我们就要处理数据的问题
    //然后有实现，写入逻辑 -> 依次拷贝数据的时候，指针移动需要检查是否越界
  
    //写入数据的应该是
    //虽然时候我们还需要计算出剩余的数据块
    //下面几行怎么得到空闲的长度
    //需要计算写指针-读指针，因为原来的写指针已经有了-去掉的数据，剩下的数据就是剩下的，然后有通过总长度减去
    //但是需要注意一个问题，问题就是我们是环形缓冲区，所以说缓冲区的写指针实际的值可能比读指针小，所以说写指针可能已经绕过去一圈了，所以需要那个镜像位来判断实际的走过了
    unsigned int data_length = static_cast<unsigned int>(this->data_space());
    unsigned int empty_data_length = this->size - data_length;

    unsigned int actually_data_length = (length < empty_data_length) ? length : empty_data_length;
    //这一个比较重要的问题，我们需要算出剩余的空间和本次写入需要的空间谁大，也就是说是否有足够的时间把需要写的写完，就是我们正常使用的空间
    //如果数据的长度要求其实已经大大的已经超出实际的空间，那我们也只能写入剩余空间，那剩余没有放进去的数据怎么办 返回1->抛出异常的方式
    //我们可以选择保存，也就是当数据被覆盖之后，再继续写入->其实可以其实不推荐做

    //定义了这个现在我们可以得出了->环形，从那里开始写
    //测试发现的问题，环形缓冲区的逻辑是环的，但是如果按照普通数组去访问，因为内存是线性的
    //那就会出现，是否数据端会不包含缓冲区的末尾和开头的中间，就是这个时间段同样需要两个方面来进行处理
    unsigned int data_to_end_length = this->size - this->write_pos; //这里就是我们计算写指针的位置距离内存实质的上还剩下多少
        
    //现在判断是否属于数据段是连续的情况，我们就是可以直接存储
    if(actually_data_length <= data_to_end_length)
    {
        memcpy(this->buffer + this->write_pos,data,actually_data_length);
        this->write_pos += actually_data_length;//数据拷贝完成之后，直接将指针移动到指定位置下面，而不是一个一个移动太慢

        if(this->write_pos == this->size)
        {
            this->write_pos = 0;
            this->write_pos_mirror ^= 1;
        }//此时到达缓冲区末尾，翻转镜像位以及清空写指针

        return actually_data_length;
    }
    //现在就是如果数据段包含末尾，数据段空间不够存放，需要我们分段存储一个在开头，另外一个在末尾
    else
    {
        memcpy(this->buffer + this->write_pos,data,data_to_end_length);
        memcpy(this->buffer,data + data_to_end_length,actually_data_length - data_to_end_length);
        //因为已经存储了一部分了，所以要记住不要重复存储以及其他的剩余的缓冲区没有存储
        //第一个存储器在末尾，第二个存储器在开头

        this->write_pos = actually_data_length - data_to_end_length;//这样就是可以计算出写指针应该在开头的那个位置
        this->write_pos_mirror ^= 1;//既然已经回到了开头了，一定要把镜像位反转
        return actually_data_length;
    }
    
    return RINGBUFFER_WRITE_EMPTY;
}

/*
    下面是对读取的实现，读取的逻辑是读取其中的数据块同时换向
    需要判断是否为空，如果为空我们就继续读取数据，需要知道现在有几个数据，逻辑来说就是read_pos在0的位置的时候，write_pos是现在是几个，read_pos是哪个值都行
    而且我发现缓冲区需要访问read_pos的数据，所以到读取数据的时候，我们是不是直接读取需要判断
    下面我们还是需要安全检查
*/
int Ringbuffer::read(char *data,unsigned int length)
{
    if(data == nullptr || length ==0)
    {
        return RINGBUFFER_SAFE_FAIL;
    }

    //检查缓冲区是否为空
    int Ringbuffer_data_state = this->Ringbuffer_mirror_inspect();
    if(Ringbuffer_data_state == RINGBUFFER_EMPTY)
    {
        return RINGBUFFER_READ_FAIL;
    }

    //如果现在已经判断出来了，数据是有的，就判断写入的时候够不够，如果不够，我们就需要判断剩余的有效数据块，然后同样也是实际长度和这个边界长度
    unsigned int data_length = static_cast<unsigned int>(this->data_space());
    //上面这些都是全部都是写指针的大文件的数情况一样，不知道是不是需要再通过总长度减去data_length，因为此时data_length是已经没有了我们需要的数据还是这个长度

    unsigned actually_read_length = (length < data_length) ? length : data_length;
    unsigned data_to_end_pos = this->size - this->read_pos;
    //读取的时候也是完全相同的方式，区别是实际可以读取的数量还有这个边界长度

    if(actually_read_length <= data_to_end_pos)
    {
        memcpy(data,this->buffer + this->read_pos,actually_read_length);
        this->read_pos += actually_read_length;

        if(this->read_pos == this->size)
        {
            this->read_pos = 0;
            this->read_pos_mirror ^= 1;
        }

        return actually_read_length;
    }
    else
    {
        memcpy(data, this->buffer + this->read_pos, data_to_end_pos);
        memcpy(data + data_to_end_pos, this->buffer, actually_read_length - data_to_end_pos);

        this->read_pos = actually_read_length - data_to_end_pos;
        this->read_pos_mirror ^= 1;

        return actually_read_length;
    }
    //其他的写法与写入完全相似，注意读取的时候指针和写入有什么不同
}

//剩余可写空间
int Ringbuffer::free_space()
{
    return this->size - static_cast<unsigned int>(this->data_space());
}

//当前可读数据长度
int Ringbuffer::data_space()
{
    if(this->read_pos == this->write_pos)
    {
        return this->read_pos_mirror == this->write_pos_mirror ? 0 : this->size;
    }

    if(this->write_pos_mirror == this->read_pos_mirror)
    {
        return this->write_pos - this->read_pos;
    }

    return this->size - this->read_pos + this->write_pos;
}
