/*
    接下来为Can的实现文件
    作为实行文件，我们需要做好文件的具体内容实现
    思考一个好的文件该实现哪些内容
    承接main.cpp的思路，我们需要在Can.cpp中实现Can的初始化，Can的使用，Can的任务调度等
    但是Can的使用和任务调度需要依赖于Can.hpp中的类和函数的定义，所以我们需要先在Can.hpp中定义好Can的类和函数，然后在Can.cpp中实现这些类和函数
    但是在Can.hpp中我们需要定义好Can的类和函数的接口，这样才能在Can.cpp中实现这些类和函数
    这部分为C++独有，开始学习类与封装

    关键发现，在树莓派中实际上把CAN抽象为了一个SocketCAN的设备，所以我们可以通过SocketCAN来实现CAN的通信
    接下来来学习TCP同时复习socket编程，学习SocketCAN的使用
*/


#include "Can.hpp"

