#include <cstdlib>  //for malloc(),realloc()
#include <iostream>
using std::cout;
using std::endl;
using std::cerr;
#define __THROW_BAD_ALLOC   cerr << "out of memory" << endl; exit(1)

namespace MyAllocc
{
	//----------------------------------------------
	// 第1级配置器。
	//----------------------------------------------
	void(*oom_handler)() = nullptr;	//异常处理函数指针

	void* oom_malloc(size_t n)
	{
		while (true) //不断尝试释放、配置、再释放、再配置…
		{
			if (oom_handler == nullptr)
			{
				__THROW_BAD_ALLOC;
			}
			(*oom_handler)();    //企图释放
			auto result = malloc(n);        //再次尝试配置
			if (result) return result;
		}
	}
	void* oom_realloc(void *p, size_t n)
	{
		while(true) //不断尝试释放、配置、再释放、再配置…
		{    
			if (oom_handler == nullptr)
			{ 
				__THROW_BAD_ALLOC;
			}
			(*oom_handler)();
			auto result = realloc(p, n);   
			if (result) return result;
		}
	}

	void* malloc_allocate(size_t n)
	{
		void *result = malloc(n);   //直接使用 malloc()
		if (!result) result = oom_malloc(n);
		return result;
	}

	void malloc_deallocate(void* p, size_t n)
	{
		free(p);  //直接使用 free()
	}

	void* malloc_reallocate(void *p, size_t old_sz, size_t new_sz)
	{
		void* result = realloc(p, new_sz); //直接使用 realloc()
		if (!result) result = oom_realloc(p, new_sz);
		return result;
	}

	void(*set_malloc_handler(void(*f)()))()
	{ //类似 C++ 的 set_new_handler().
		auto old = oom_handler;
		oom_handler = f;
		return old;	//返回函数指针
	}

	//----------------------------------------------
	//第二级配置器
	//----------------------------------------------

	const int __ALIGN = 8;
	const int __MAX_BYTES = 128;		//小区块上限
	const int __NFREELISTS = __MAX_BYTES / __ALIGN;	//free_list个数
	const int DEFAULT_CHUNK_NUM = 20;	//默认chunk个数
	struct obj
	{
		obj* free_list_link;
	};

	char*   start_free = 0;	//战备池start
	char*   end_free = 0;	//战备池end
	size_t  heap_size = 0;	//分配的总空间
	obj* free_list_arr[__NFREELISTS] = { nullptr };

	size_t ROUND_UP(const size_t bytes)	//调整至8的倍数
	{
		return (bytes + __ALIGN - 1) & ~(__ALIGN - 1);
	}
	size_t FREELIST_INDEX(const size_t bytes)	//计算应该分配到哪个free list
	{
		return (bytes + __ALIGN - 1) / __ALIGN - 1;
	}

	//----------------------------------------------
	// We allocate memory in large chunks in order to
	// avoid fragmentingthe malloc heap too much.
	// We assume that size is properly aligned.
	//
	// Allocates a chunk for nobjs of size "size".
	// nobjs may be reduced if it is inconvenient to
	// allocate the requested number.
	//----------------------------------------------
	char* chunk_alloc(size_t size, int& nobjs) //size<=128
	{
		char* result;
		size_t total_bytes = size * nobjs;
		size_t bytes_left = end_free - start_free;	//战备池大小

		if (bytes_left >= total_bytes)	
		{
			result = start_free;
			start_free += total_bytes;
			return result;
		}
		else if (bytes_left >= size)		//有剩余空间 分割战备池
		{
			nobjs = bytes_left / size;	//重新计算分块数量 pass by reference
			total_bytes = size * nobjs;
			result = start_free;		
			start_free += total_bytes;	//移动start游标
			return result;
		}
		else
		{
			size_t bytes_to_get =
				2 * total_bytes + ROUND_UP(heap_size >> 4);		//扩充2倍+偏移量
			// Try to make use of the left-over piece.
			if (bytes_left > 0)	//战备池剩余大小>=8，把他分配到free_list中
			{
				obj* volatile *my_free_list =
					free_list_arr + FREELIST_INDEX(bytes_left);

				((obj*)start_free)->free_list_link = *my_free_list;		
				*my_free_list = (obj*)start_free;
			}
			start_free = (char*)malloc(bytes_to_get);	//malloc扩容内存池
			if (start_free == nullptr)		//malloc失败，试图从内存池中重新分配
			{
				obj* volatile *my_free_list, *p;

				//Try to make do with what we have. That can't
				//hurt. We do not try smaller requests, since that tends
				//to result in disaster on multi-process machines.	
				for (int i = size; i <= __MAX_BYTES; i += __ALIGN)	//遍历free list 找到右边最近的有空闲的块
				{
					my_free_list = free_list_arr + FREELIST_INDEX(i);
					p = *my_free_list;
					if (0 != p)
					{
						*my_free_list = p->free_list_link;
						start_free = (char*)p;		//调整战备池指向
						end_free = start_free + i;	//调整战备池空间
						return chunk_alloc(size, nobjs);		//战备池有空间了
						//Any leftover piece will eventually make it to the
						//right free list.
					}
				}
				end_free = 0;       //In case of exception.
				start_free = (char*)malloc_allocate(bytes_to_get);
				//This should either throw an exception or
				//remedy the situation. Thus we assume it
				//succeeded.
			}
			heap_size += bytes_to_get;	//新申请的空间
			end_free = start_free + bytes_to_get;	//移动end节点
			return chunk_alloc(size, nobjs);		//战备池有空间了 递归调用一次将内存分配出去
		}
	}
	//----------------------------------------------
	// Returns an object of size n, and optionally adds
	// to size n free list. We assume that n is properly aligned.
	// We hold the allocation lock.
	//----------------------------------------------
	void* refill(size_t n) // n<=128
	{
		int nobjs = DEFAULT_CHUNK_NUM;	//默认分配20个块，不够则减小
		char* chunk = chunk_alloc(n, nobjs);
		obj* volatile *my_free_list;   //obj** my_free_list;
		obj* result;
		obj* current_obj;
		obj* next_obj;

		if (1 == nobjs) return chunk;	//只有一个块 直接分出去
		my_free_list = free_list_arr + FREELIST_INDEX(n);

		//Build free list in chunk
		result = (obj*)chunk;
		*my_free_list = next_obj = (obj*)(chunk + n);
		for (int i = 1;; ++i)	//将得到的空间使用嵌入式指针串接起来
		{
			current_obj = next_obj;
			next_obj = (obj*)((char*)next_obj + n);	//n为8的倍数 保证固定的偏移量
			if (nobjs - 1 == i)
			{
				current_obj->free_list_link = nullptr;	//剩下的空间留给战备池
				break;
			}
			else
			{
				current_obj->free_list_link = next_obj;	
			}
		}
		return result;
	}
	
	//----------------------------------------------
	// 从内存池分配
	//----------------------------------------------
	void* allocate(const size_t n)  //n must be > 0
	{
		if (n > (size_t)__MAX_BYTES)		//如果大于小区块上限 调用默认malloc
		{
			return malloc_allocate(n);
		} 
		auto & my_free_list = free_list_arr[FREELIST_INDEX(n)];	//判断分配到哪个链表
		if (my_free_list == nullptr)
		{
			return refill(ROUND_UP(n));		//当前链表为空 分配空间
		}
		auto ans = my_free_list;
		my_free_list = my_free_list->free_list_link;	//往下走
		return ans;
	}
	//----------------------------------------------
	//回收
	//----------------------------------------------
	void deallocate(void *p, const size_t n)  //p may not be 0
	{
		if (!p)
			return;
		obj* q = (obj*)p;
		obj* volatile *my_free_list;   //obj** my_free_list;

		if (n > (size_t)__MAX_BYTES)		
		{
			malloc_deallocate(p, n);		//回收到操作系统
			return;
		}
		my_free_list = free_list_arr + FREELIST_INDEX(n);	
		q->free_list_link = *my_free_list;	//新链表插入到free list头部
		*my_free_list = q;	//头节点更新 回收到内存池
	}
	//----------------------------------------------
	//重新分配大小
	//----------------------------------------------
	void* reallocate(void *p, const size_t old_sz, const size_t new_sz)
	{
		void * result;
		size_t copy_sz;

		if (old_sz > (size_t)__MAX_BYTES && new_sz > (size_t)__MAX_BYTES) 
		{
			return realloc(p, new_sz);
		}
		if (ROUND_UP(old_sz) == ROUND_UP(new_sz)) return(p);
		result = allocate(new_sz);
		copy_sz = new_sz > old_sz ? old_sz : new_sz;
		memcpy(result, p, copy_sz);
		deallocate(p, old_sz);
		return result;
	}
	//----------------------------------------------

	void PrintInfo()
	{
		cout << "当前内存池大小" + heap_size << endl;
	}
};

