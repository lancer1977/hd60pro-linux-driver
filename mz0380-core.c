/*
 *  Driver for MZ0380 based capture cards.
 */

#include <linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>

#include "mz0380-internal.h"

MODULE_DESCRIPTION("Driver for MZ0380 based capture cards");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");
