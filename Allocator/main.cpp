#include <cstdlib>
#include <iostream>
#include "allocc.h"
#include <vector>
#include <list>
using namespace std;

//void * operator new (size_t s)
//{
//	cout << "test" << endl;
//	return malloc(s);
//}
int main()
{
	int* buf[100];
	for (int i = 0; i < 100; ++i)
	{
		buf[i] = (int*)MyAllocc::allocate(4);
	}
	for (int i = 0; i < 100; ++i)
	{
		cout << buf[i]<<endl;
	}
	return 0;
}