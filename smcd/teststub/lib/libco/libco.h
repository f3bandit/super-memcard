// Test stand-in for Main's libco (shimtest only): one thread, no coroutines
#pragma once
typedef void *cothread_t;
cothread_t co_active();
