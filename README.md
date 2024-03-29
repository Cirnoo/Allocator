# Allocator  

 mini memery pool from STL

## 简介  

从SGI版本STL allocc中提取出来的内存池，对其进行了改写去除了线程安全和泛型部分便于学习

## 内存池作用  

1. 减少malloc/free次数，从而减少内存碎片和new对象产生的额外空间开销（cookie等）
2. 避免内存泄漏  

## 原理

简而言之就是一次性申请一大片内存空间，根据需要对其进行切割，从而减小malloc次数  

## 具体实现

### 嵌入式指针

STL中使用嵌入式指针管理内存块的连接，使用union让链表的next指针和实际数据共用内存，当内存块没有分配出去的时候表现为指针，当数据分配出去后内存块与内存池分离，所以不需要next指针了，转为实际数据，这样使得数据与指针公用了内存空间，节省了一个指针的大小，前提是数据块大小不能小于指针的大小。

这样做的好处是节省了一个指针的内存空间，坏处是无法将内存还给操作系统。  

```cpp
union obj
{
　　union obj *free_list_link;
　　char client_data[1];
};
```

在本项目的代码中因为去除了泛型的部分所以直接改为了结构体，原理与STL相同

```cpp
struct obj
{
	obj* free_list_link;
};
```

### free list

在全局区域内设置有16条free list，从第0个到第15个管理内存块以8的倍数增长（8，16，24...128）,当使用者需要new一块内存时，内存池首先会将需要的内存大小向上调整至8的倍数，当内存块大小大于free list所管理的最大值（128byte）时，直接调用malloc函数，否则会检索对应的free list上是否挂有空闲的内存块，如果有则直接分给使用者一块空间，并移动头指针，指向下一个空闲节点。  

![free list](https://raw.githubusercontent.com/Cirnoo/Allocator/master/Img/free_list.png) 
 

### 向内存池充值  

加入要申请一个n大小的内存，首先n会被上调至8的倍数，如果free list中空闲块为空，则会调用refill函数向内存池充值空间，refill函数首先会检查备用池（战备池）是否有空闲空间，如果有则按当前申请的大小分割战备池，最大不超过20块，分割完毕后加入到free list中管理。如果战备池的空间也不够，则调用malloc分配一大块空间，其中20*n字节的空间被分配到free list中，剩下的分配到战备池中。  

### 备用内存池（战备池）

在内存池向操作系统申请内存的时候会一次性申请`size_t bytes_to_get = 2 * total_bytes + ROUND_UP(heap_size >> 4);`大小的内存，total_bytes为分配给free list的空间，剩下的空间会分配到战备池，有一个start和end指针分别指向战备池的头和尾，在使用者向内存池申请内存的时候，free list会优先考虑战备池是否还有空闲的内存，如果有则加入到free list中，如果战备池剩余大小不足以分配当前所需的内存，则会将战备池的内存挂到较小的free list中，避免内存泄漏，再重新给战备池充值。

![pool](https://raw.githubusercontent.com/Cirnoo/Allocator/master/Img/pool.png)  


### 当系统内存不足时

如果内存池使用malloc申请空间失败了，那么会想右边的free list进行查找，如果有空闲的内存块，那么可以拿出来分配到战备池中，再分配给使用者。如果右边的free list也没有空闲的内存块了，那么抛出异常。  

### 向内存池申请内存  

调用内存池的`allocate`函数可以获得一块内存，注意这块内存会被上调至8的倍数（在大小小于128byte的前提下）。

### 内存回收  

调用`deallocate`会将内存回收到内存池，但是并不会回收到操作系统，如何回收到操作系统需要进一步考虑。  
