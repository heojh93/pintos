#include "page.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include <string.h>


static unsigned vm_hash_func (const struct hash_elem *, void * UNUSED);
static bool vm_less_func (const struct hash_elem *, const struct hash_elem *, void * UNUSED);
static void vm_destroy_func (struct hash_elem *, void * UNUSED);

void
vm_init (struct hash *vm)
{
	hash_init (vm, vm_hash_func, vm_less_func, NULL);
}

void vm_destroy (struct hash *vm)
{
	hash_destroy (vm, vm_destroy_func);
}

static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
	return hash_int (hash_entry (e, struct vm_entry, elem)->vaddr);
}

static bool
vm_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
	struct vm_entry *vme1 = hash_entry (a, struct vm_entry, elem);
	struct vm_entry *vme2 = hash_entry (b, struct vm_entry, elem);
	return vme1->vaddr < vme2->vaddr;
}

static void
vm_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
	struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);
	struct thread *t = thread_current();

	if(vme->is_loaded){
		palloc_free_page(pagedir_get_page(t->pagedir, vme->vaddr));
		pagedir_clear_page(t->pagedir, vme->vaddr);
	}
	free(vme);
}

struct vm_entry*
find_vme (void *vaddr)
{
	struct vm_entry vme;
	struct hash_elem *e;

	vme.vaddr = pg_round_down(vaddr);
	e = hash_find(&thread_current()->vm, &vme.elem);

	return (!e)? NULL : hash_entry (e,struct vm_entry,elem);
}

bool 
insert_vme (struct hash *vm, struct vm_entry *vme)
{
	struct hash_elem *e = &vme->elem;
	
	return (!hash_insert(vm,e))? true : false;
}

bool
delete_vme (struct hash *vm, struct vm_entry *vme)
{
	struct hash_elem *e = &vme->elem;

	return (!hash_delete(vm,e))? false : true;
}

bool
load_file (void *kaddr, struct vm_entry *vme)
{
	if(file_read_at(vme->file, kaddr,vme->read_bytes,vme->offset)
			!= (off_t)(vme->read_bytes)){
		return false;
	}
	memset(kaddr + vme->read_bytes, 0, vme->zero_bytes);
	return true;
}

