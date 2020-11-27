// Copyright (c) 2013, Facebook, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name Facebook nor the names of its contributors may be used to
//     endorse or promote products derived from this software without specific
//     prior written permission.
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "fishhook.h"

#include <dlfcn.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <mach/vm_region.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

#ifdef __LP64__
typedef struct mach_header_64 mach_header_t;
typedef struct segment_command_64 segment_command_t;
typedef struct section_64 section_t;
typedef struct nlist_64 nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT_64
#else
typedef struct mach_header mach_header_t;
typedef struct segment_command segment_command_t;
typedef struct section section_t;
typedef struct nlist nlist_t;
#define LC_SEGMENT_ARCH_DEPENDENT LC_SEGMENT
#endif

#ifndef SEG_DATA_CONST
#define SEG_DATA_CONST  "__DATA_CONST"
#endif

///一个链表  当next为nil 代表是 第一次绑定
struct rebindings_entry {
  struct rebinding *rebindings;// rebinding 数组实例
  size_t rebindings_nel;// 元素数量
  struct rebindings_entry *next; // 链表索引
};

///链表的表头
static struct rebindings_entry *_rebindings_head;

static int prepend_rebindings(struct rebindings_entry **rebindings_head,
                              struct rebinding rebindings[],
                              size_t nel) {
  // 声明 rebindings_entry 一个指针，并为其分配空间
  struct rebindings_entry *new_entry = (struct rebindings_entry *) malloc(sizeof(struct rebindings_entry));
  if (!new_entry) {
      // 分配空间失败的容错处理
    return -1;
  }
    // 为链表中元素的 rebindings 实例分配指定空间
  new_entry->rebindings = (struct rebinding *) malloc(sizeof(struct rebinding) * nel);
  if (!new_entry->rebindings) {
      // 分配空间失败的容错处理
    free(new_entry);
    return -1;
  }
    // 将 rebindings 数组中 copy 到 new_entry -> rebingdings 成员中
  memcpy(new_entry->rebindings, rebindings, sizeof(struct rebinding) * nel);
    ///更新链表的数量
  new_entry->rebindings_nel = nel;
  //添加新增的指针
  new_entry->next = *rebindings_head;
  /// 将 rebindings_head 设置为表头
  *rebindings_head = new_entry;
  return 0;
}

static vm_prot_t get_protection(void *sectionStart) {
  mach_port_t task = mach_task_self();
  vm_size_t size = 0;
  vm_address_t address = (vm_address_t)sectionStart;
  memory_object_name_t object;
#if __LP64__
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
  vm_region_basic_info_data_64_t info;
  kern_return_t info_ret = vm_region_64(
      task, &address, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_64_t)&info, &count, &object);
#else
  mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT;
  vm_region_basic_info_data_t info;
  kern_return_t info_ret = vm_region(task, &address, &size, VM_REGION_BASIC_INFO, (vm_region_info_t)&info, &count, &object);
#endif
  if (info_ret == KERN_SUCCESS) {
    return info.protection;
  } else {
    return VM_PROT_READ;
  }
}

///重新绑定操作
static void perform_rebinding_with_section(struct rebindings_entry *rebindings,
                                           section_t *section,
                                           intptr_t slide,
                                           nlist_t *symtab,
                                           char *strtab,
                                           uint32_t *indirect_symtab) {
    // 在 Indirect Symbol 表中检索到对应位置
  const bool isDataConst = strcmp(section->segname, SEG_DATA_CONST) == 0;
  uint32_t *indirect_symbol_indices = indirect_symtab + section->reserved1;
  // 获取 _DATA.__nl_symbol_ptr(或__la_symbol_ptr) Section
  // 已知其 value 是一个指针类型，整段区域用二阶指针来获取
  void **indirect_symbol_bindings = (void **)((uintptr_t)slide + section->addr);
  vm_prot_t oldProtection = VM_PROT_READ;
  if (isDataConst) {
    oldProtection = get_protection(rebindings);
    mprotect(indirect_symbol_bindings, section->size, PROT_READ | PROT_WRITE);
  }
  //用 size / 一阶指针来计算个数，遍历整个 Section
  for (uint i = 0; i < section->size / sizeof(void *); i++) {
      // 通过下标来获取每一个 Indirect Address 的 Value // 这个 Value 也是外层寻址时需要的下标
    uint32_t symtab_index = indirect_symbol_indices[i];
    if (symtab_index == INDIRECT_SYMBOL_ABS || symtab_index == INDIRECT_SYMBOL_LOCAL ||
        symtab_index == (INDIRECT_SYMBOL_LOCAL   | INDIRECT_SYMBOL_ABS)) {
      continue;
    }
     // 获取符号名在字符表中的偏移地址
    uint32_t strtab_offset = symtab[symtab_index].n_un.n_strx;
      // 获取符号名
    char *symbol_name = strtab + strtab_offset;
      // 过滤掉符号名小于 4 位的符号
    bool symbol_name_longer_than_1 = symbol_name[0] && symbol_name[1];
      // 取出 rebindings 结构体实例数组，开始遍历链表
    struct rebindings_entry *cur = rebindings;
      // 对于链表中每一个 rebindings 数组的每一个 rebinding 实例
      // 依次在 String Table 匹配符号名
    while (cur) {
         // 符号名与方法名匹配
      for (uint j = 0; j < cur->rebindings_nel; j++) {
        if (symbol_name_longer_than_1 &&
            strcmp(&symbol_name[1], cur->rebindings[j].name) == 0) {
            // 如果是第一次对跳转地址进行重写
          if (cur->rebindings[j].replaced != NULL &&
              indirect_symbol_bindings[i] != cur->rebindings[j].replacement) {
              // 记录原始跳转地址
            *(cur->rebindings[j].replaced) = indirect_symbol_bindings[i];
          }
            // 重写跳转地址
          indirect_symbol_bindings[i] = cur->rebindings[j].replacement;
            // 完成后不再对当前 Indirect Symbol 处理 // 继续迭代到下一个 Indirect Symbol
          goto symbol_loop;
        }
      }
         // 链表遍历
      cur = cur->next;
    }
  symbol_loop:;
  }
  if (isDataConst) {
    int protection = 0;
    if (oldProtection & VM_PROT_READ) {
      protection |= PROT_READ;
    }
    if (oldProtection & VM_PROT_WRITE) {
      protection |= PROT_WRITE;
    }
    if (oldProtection & VM_PROT_EXECUTE) {
      protection |= PROT_EXEC;
    }
    mprotect(indirect_symbol_bindings, section->size, protection);
  }
}

//这段方法主要描述了替换 __DATA.__la_symbol_ptr 和 __DATA.__la_symbol_ptr 的 Indirect Pointer 主要过程。从 reserved1 字段获取到 Indirect Symbols 对应的位置。从中我们可以获取到指定符号的偏移量，这个偏移量主要用来在 String Table 中检索出符号名称字符串。之后我们找到 __DATA.__la_symbol_ptr 和 __DATA.__la_symbol_ptr 这两个 Section。这两个表中，都是由 Indirect Pointer 构成的指针数组，但是其中的元素决定了我们调用的方法应该以哪个代码段的方法来执行。我们遍历这个指针数组中每一个指针，在每一层遍历中取出其符号名称，与我们的 rebindings 链表中每一个元素进行比对，当名称匹配的时候，重写其指向地址

static void rebind_symbols_for_image(struct rebindings_entry *rebindings,
                                     const struct mach_header *header,
                                     intptr_t slide) {
  Dl_info info;
  if (dladdr(header, &info) == 0) {
    return;
  }

  segment_command_t *cur_seg_cmd;
  segment_command_t *linkedit_segment = NULL;
  struct symtab_command* symtab_cmd = NULL;
  struct dysymtab_command* dysymtab_cmd = NULL;

    // 初始化游标
    // header = 0x100000000 - 二进制文件基址默认偏移
    // sizeof(mach_header_t) = 0x20 - Mach-O Header 部分
    // 首先需要跳过 Mach-O Header
  uintptr_t cur = (uintptr_t)header + sizeof(mach_header_t);
    
    // 遍历每一个 Load Command，游标每一次偏移每个命令的 Command Size 大小
    // header -> ncmds: Load Command 加载命令数量
    // cur_seg_cmd -> cmdsize: Load 大小
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
      // 取出当前的 Load Command
    cur_seg_cmd = (segment_command_t *)cur;
      // Load Command 的类型是 LC_SEGMENT 将文件中（32位或64位）的段映射到进程地址空间中
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
        // 比对一下 Load Command 的 name 是否为 __LINKEDIT
      if (strcmp(cur_seg_cmd->segname, SEG_LINKEDIT) == 0) {
          // 检索到 __LINKEDIT
        linkedit_segment = cur_seg_cmd;
      }
//        LC_SEGMENT - 代表当前区域链接器信息
    } else if (cur_seg_cmd->cmd == LC_SYMTAB) {//符号地址 LC_SYMTAB
      symtab_cmd = (struct symtab_command*)cur_seg_cmd;
    } else if (cur_seg_cmd->cmd == LC_DYSYMTAB) {
        // 判断当前 Load Command 是否是 LC_DYSYMTAB 类型
        // LC_DYSYMTAB - 代表动态链接器信息区域
        // 检索到 LC_DYSYMTAB
      dysymtab_cmd = (struct dysymtab_command*)cur_seg_cmd;
    }
  }

    // 不是动态链接器信息区域 | 代表当前区域链接器信息 | 符号地址
  if (!symtab_cmd || !dysymtab_cmd || !linkedit_segment ||
      !dysymtab_cmd->nindirectsyms) {
    return;
  }

    // slide: ASLR 偏移量
    // vmaddr: SEG_LINKEDIT 的虚拟地址
    // fileoff: SEG_LINKEDIT 地址偏移
    // 式①：base = SEG_LINKEDIT真实地址 - SEG_LINKEDIT地址偏移
    // 式②：SEG_LINKEDIT真实地址 = SEG_LINKEDIT虚拟地址 + ASLR偏移量
    // 将②代入①：Base = SEG_LINKEDIT虚拟地址 + ASLR偏移量 - SEG_LINKEDIT地址偏移
  // Find base symbol/string table addresses
  uintptr_t linkedit_base = (uintptr_t)slide + linkedit_segment->vmaddr - linkedit_segment->fileoff;
    
    // 通过 base + symtab 的偏移量 计算 symtab 表的首地址，并获取 nlist_t 结构体实例
  nlist_t *symtab = (nlist_t *)(linkedit_base + symtab_cmd->symoff);
    
    // 通过 base + stroff 字符表偏移量计算字符表中的首地址，获取字符串表
  char *strtab = (char *)(linkedit_base + symtab_cmd->stroff);

    // 通过 base + indirectsymoff 偏移量来计算动态符号表的首地址
  // Get indirect symbol table (array of uint32_t indices into symbol table)
  uint32_t *indirect_symtab = (uint32_t *)(linkedit_base + dysymtab_cmd->indirectsymoff);

// 归零游标，复用
  cur = (uintptr_t)header + sizeof(mach_header_t);
    // 再次遍历 Load Commands
  for (uint i = 0; i < header->ncmds; i++, cur += cur_seg_cmd->cmdsize) {
    cur_seg_cmd = (segment_command_t *)cur;
      // Load Command 的类型是 LC_SEGMENT
    if (cur_seg_cmd->cmd == LC_SEGMENT_ARCH_DEPENDENT) {
      if (strcmp(cur_seg_cmd->segname, SEG_DATA) != 0 &&
          strcmp(cur_seg_cmd->segname, SEG_DATA_CONST) != 0) {
           // 查询 Segment Name 过滤出 __DATA 或者 __DATA_CONST
        continue;
      }
    
        // 遍历 Segment 中的 Section
      for (uint j = 0; j < cur_seg_cmd->nsects; j++) {
          // 取出 Section
        section_t *sect =
          (section_t *)(cur + sizeof(segment_command_t)) + j;
          // flags & SECTION_TYPE 通过 SECTION_TYPE 掩码获取 flags 记录类型的 8 bit
          // 如果 section 的类型为 S_LAZY_SYMBOL_POINTERS
          // 这个类型代表 lazy symbol 指针 Section
        if ((sect->flags & SECTION_TYPE) == S_LAZY_SYMBOL_POINTERS) {
            // 进行 rebinding 重写操作
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
          // 这个类型代表 non-lazy symbol 指针 Section
        if ((sect->flags & SECTION_TYPE) == S_NON_LAZY_SYMBOL_POINTERS) {
          perform_rebinding_with_section(rebindings, sect, slide, symtab, strtab, indirect_symtab);
        }
      }
    }
  }
}

static void _rebind_symbols_for_image(const struct mach_header *header,
                                      intptr_t slide) {
    rebind_symbols_for_image(_rebindings_head, header, slide);
}

int rebind_symbols_image(void *header,
                         intptr_t slide,
                         struct rebinding rebindings[],
                         size_t rebindings_nel) {
    struct rebindings_entry *rebindings_head = NULL;
    int retval = prepend_rebindings(&rebindings_head, rebindings, rebindings_nel);
    rebind_symbols_for_image(rebindings_head, (const struct mach_header *) header, slide);
    if (rebindings_head) {
      free(rebindings_head->rebindings);
    }
    free(rebindings_head);
    return retval;
}

///函数开头
int rebind_symbols(struct rebinding rebindings[], size_t rebindings_nel) {
    // 维护一个 rebindings_entry 的结构
    // 将 rebinding 的多个实例组织成一个链表
  int retval = prepend_rebindings(&_rebindings_head, rebindings, rebindings_nel);// 判断是否 malloc 失败，失败会返回 -1
  if (retval < 0) {
    return retval; // _rebindings_head -> next 是第一次调用的标志符，NULL 则代表第一次调用
  }
  // If this was the first call, register callback for image additions (which is also invoked for
  // existing images, otherwise, just run on existing images
  if (!_rebindings_head->next) {// 第一次调用，将 _rebind_symbols_for_image 注册为回调
    _dyld_register_func_for_add_image(_rebind_symbols_for_image);
  } else { // 先获取 dyld 镜像数量
    uint32_t c = _dyld_image_count();
    for (uint32_t i = 0; i < c; i++) {// 根据下标依次进行重绑定过程
      _rebind_symbols_for_image(_dyld_get_image_header(i), _dyld_get_image_vmaddr_slide(i));
    }
  }// 返回状态值
  return retval;
}


