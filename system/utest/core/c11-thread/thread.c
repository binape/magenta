// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>

#include <magenta/syscalls.h>
#include <unittest/unittest.h>

volatile int threads_done[7];

static int thread_entry(void* arg) {
    int thread_number = (int)(intptr_t)arg;
    errno = thread_number;
    unittest_printf("thread %d sleeping for .1 seconds\n", thread_number);
    mx_nanosleep(100 * 1000 * 1000);
    EXPECT_EQ(errno, thread_number, "errno changed by someone!");
    threads_done[thread_number] = 1;
    return thread_number;
}

bool c11_thread_test(void) {
    BEGIN_TEST;

    thrd_t thread;
    int return_value;

    unittest_printf("Welcome to thread test!\n");

    for (int i = 0; i != 4; ++i) {
        int ret = thrd_create_with_name(&thread, thread_entry, (void*)(intptr_t)i, "c11 thread test");
        ASSERT_EQ(ret, thrd_success, "Error while creating thread");

        ret = thrd_join(thread, &return_value);
        ASSERT_EQ(ret, thrd_success, "Error while thread join");
        // TODO(kulakowski) Fix thread return values.
        // ASSERT_EQ(return_value, i, "Incorrect return from thread");
    }

    unittest_printf("Attempting to create thread with a super long name. This should fail\n");
    int ret = thrd_create_with_name(&thread, thread_entry, NULL,
                                    "01234567890123456789012345678901234567890123456789012345678901234567890123456789");
    ASSERT_NEQ(ret, thrd_success, "thread creation should have thrown error");

    unittest_printf("Attempting to create thread with a null name. This should succeed\n");
    ret = thrd_create_with_name(&thread, thread_entry, (void*)(intptr_t)4, NULL);
    ASSERT_EQ(ret, thrd_success, "Error returned from thread creation");

    ret = thrd_join(thread, &return_value);
    ASSERT_EQ(ret, thrd_success, "Error while thread join");
    // TODO(kulakowski) Fix thread return values.
    // ASSERT_EQ(return_value, 4, "Incorrect return from thread");

    ret = thrd_create_with_name(&thread, thread_entry, (void*)(intptr_t)5, NULL);
    ASSERT_EQ(ret, thrd_success, "Error returned from thread creation");
    ret = thrd_detach(thread);
    ASSERT_EQ(ret, thrd_success, "Error while thread detach");

    while (!threads_done[5])
        mx_nanosleep(100 * 1000 * 1000);

    thread_entry((void*)(intptr_t)6);
    ASSERT_TRUE(threads_done[6], "All threads should have completed")

    END_TEST;
}

BEGIN_TEST_CASE(c11_thread_tests)
RUN_TEST(c11_thread_test)
END_TEST_CASE(c11_thread_tests)

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
#endif