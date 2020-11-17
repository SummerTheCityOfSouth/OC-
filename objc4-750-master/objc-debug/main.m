//
//  main.m
//  objc-debug
//
//  Created by 灏 孙  on 2018/12/6.
//

#import <Foundation/Foundation.h>
#import "TestObject.h"
#import <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/getsect.h>
//#include <Darwin/POSIX.sys.stat.h>


int main(int argc, const char * argv[]) {
    @autoreleasepool {
        // insert code here...
        NSLog(@"Hello, World!");
        NSObject *o = [[NSObject alloc] init];
        TestObject *obj = [[TestObject alloc] init];
        [obj isKindOfClass:[NSArray class]];
    }
    return 0;
}

//const struct mach_header *machHeader = NULL;
//static NSString *configuration = @"";
////设置"__DATA,__customSection"的数据为kyson
//char *kString __attribute__((section("__DATA,__customSection"))) = (char *)"kyson";
//char *kkString __attribute__((section("__DATA,__customSection"))) = (char *)"typesss";
//int main(int argc, const char * argv[]) {
//    @autoreleasepool {
//        //设置machheader信息
//        if (machHeader == NULL)
//        {
//            Dl_info info;
//            dladdr((__bridge const void *)(configuration), &info);
//            machHeader = (struct mach_header_64*)info.dli_fbase;
//        }
//
//        unsigned long byteCount = 0;
//        uintptr_t* data = (uintptr_t *) getsectiondata(machHeader, "__DATA", "__customSection", &byteCount);
//        uintptr_t* dataS = (uintptr_t *)getsectiondata(machHeader, "__DATA", "__customSection", &byteCount);
//        NSUInteger counter = byteCount/sizeof(void*);
//        for(NSUInteger idx = 0; idx < counter; ++idx)
//        {
//            char *string = (char*)data[idx];
//            NSString *str = [NSString stringWithUTF8String:string];
//            NSLog(@"%@",str);
//        }
//
//    }
//    return 0;
//}

//int main(int argc, const char *argv[]) {
//    for (int i = 1; i < argc; ++i) {
//        if (!processFile(argv[i])) return 1;
//    }
//    mmap
//    return 0;
//}
//
//bool processFile(const char *filename)
//{
//    if (debug) printf("file %s\n", filename);
//    //打开文件
//    int fd = open(filename, O_RDWR);
//    if (fd < 0) {
//        printf("open %s: %s\n", filename, strerror(errno));
//        return false;
//    }
//
//    struct stat st;
//    //获取文件状态
//    if (fstat(fd, &st) < 0) {
//        printf("fstat %s: %s\n", filename, strerror(errno));
//        return false;
//    }
//    //将文件映射进内存进行处理
//    void *buffer = mmap(NULL, (size_t)st.st_size, PROT_READ|PROT_WRITE,
//                        MAP_FILE|MAP_SHARED, fd, 0);
//    if (buffer == MAP_FAILED) {
//        printf("mmap %s: %s\n", filename, strerror(errno));
//        return false;
//    }
//    //开始处理进入内存后的文件
//    bool result = parse_fat((uint8_t *)buffer, (size_t)st.st_size);
//    //解除映射关系
//    munmap(buffer, (size_t)st.st_size);
//    close(fd);
//    return result;
//}
