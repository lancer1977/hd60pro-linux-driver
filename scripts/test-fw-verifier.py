#!/usr/bin/env python3
"""Compile the actual verifier with fake MMIO; never accesses hardware."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'src/mz0380-fw.c').read_text()
start = source.index('static u32 mz0380_fw_upload_expected_word')
end = source.index('\n/*', start)
functions = source[start:end]
stub = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
#define U32_MAX UINT32_MAX
#define MZ0380_FW_VERIFY_SAMPLES 17
#define MZ0380_FW_VERIFY_TIMEOUT_MS 2000
#define MZ0380_FW_UPLOAD_MB_FW_BUFFER 0x60
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#define min_t(t,a,b) ((t)(a)<(t)(b)?(t)(a):(t)(b))
#define msecs_to_jiffies(x) (x)
#define jiffies_to_msecs(x) ((unsigned)(x))
#define time_after_eq(a,b) ((long)((a)-(b)) >= 0)
#define rmb() ((void)0)
#define pr_info(...) ((void)0)
#define pr_err(...) ((void)0)
struct mz0380_dev { const char *name; };
static unsigned long jiffies;
static u8 *payload;
static size_t length, reads, offsets[17], fail_at, ones_at;
static unsigned delay;
static u32 get_unaligned_le32(const void *ptr) {
    const u8 *p=ptr;
    return (u32)p[0]|((u32)p[1]<<8)|((u32)p[2]<<16)|((u32)p[3]<<24);
}
static u32 mz_mmio_read(struct mz0380_dev *dev, size_t reg) {
    (void)dev;
    size_t off=reg-0x60;
    assert(reads<17 && off<length && off%4==0);
    offsets[reads++]=off;
    u8 b[4]={0};
    memcpy(b,payload+off,length-off<4?length-off:4);
    jiffies+=delay;
    if(reads==ones_at) return UINT32_MAX;
    return get_unaligned_le32(b) ^ (reads==fail_at?1:0);
}
'''
cases = r'''
int main(void) {
    struct mz0380_dev dev={"mock"};
    payload=malloc(936192); assert(payload);
    for(size_t i=0;i<936192;i++) payload[i]=(u8)(i%251);
    length=0; assert(mz0380_fw_upload_verify(&dev,payload,0)==-EINVAL);
    assert(reads==0);
    size_t sizes[]={1,2,3,4,5,63,64,65,68,69,936191};
    for(size_t s=0;s<sizeof(sizes)/sizeof(sizes[0]);s++) {
        length=sizes[s]; reads=0; jiffies=0; delay=0; fail_at=ones_at=0;
        assert(mz0380_fw_upload_verify(&dev,payload,length)==0);
        size_t n=reads;
        assert(n<=17 && offsets[0]==0 && offsets[n-1]==((length-1)/4)*4);
        for(size_t i=1;i<n;i++) assert(offsets[i]>offsets[i-1]);
        for(size_t k=1;k<=n;k++) {
            reads=0; fail_at=k;
            assert(mz0380_fw_upload_verify(&dev,payload,length)==-EIO);
            assert(reads==k);
        }
    }
    length=936191; reads=0; fail_at=0; ones_at=1;
    memset(payload,255,4); /* all ones must fail even if expected */
    assert(mz0380_fw_upload_verify(&dev,payload,length)==-EIO && reads==1);
    memset(payload,0,4); ones_at=0; reads=0; jiffies=0; delay=2000;
    assert(mz0380_fw_upload_verify(&dev,payload,length)==-ETIMEDOUT && reads==1);
    reads=0; jiffies=0; delay=500;
    assert(mz0380_fw_upload_verify(&dev,payload,length)==-ETIMEDOUT && reads==4);
    free(payload);
    puts("PASS: endpoints, tails, 17-read cap, early mismatch/all-ones abort, deadlines");
}
'''
with tempfile.TemporaryDirectory(prefix='fw-verifier-test-') as directory:
    executable = str(Path(directory) / 'test')
    subprocess.run(['cc', '-x', 'c', '-std=c11', '-Wall', '-Wextra',
                    '-fsanitize=address,undefined', '-o', executable, '-'],
                   input=stub + functions + cases, text=True, check=True)
    subprocess.run([executable], check=True)
