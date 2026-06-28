__thread int a = 5;
__thread int b;
__thread long long c = 100;
long long compute(void){ b=7; return a+b+c; }
