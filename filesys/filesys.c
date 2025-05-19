#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* 	파일 시스템 모듈을 시작한다.
	만약 포맷이 true라면 파일시스템을 다시 포맷한다.*/
void
filesys_init (bool format) {
	filesys_disk = disk_get (0, 1);
	if (filesys_disk == NULL)
		PANIC ("hd0:1 (hdb) not present, file system initialization failed");

	inode_init ();

#ifdef EFILESYS
	fat_init ();

	if (format)
		do_format ();

	fat_open ();
#else
	/* Original FS */
	free_map_init ();

	if (format)
		do_format ();

	free_map_open ();
#endif
}

/*	파일 시스템 모듈을 닫고 아직 디스크에 기록되지 않은 모든 데이터를 디스크에 쓴다. */
void
filesys_done (void) {
	/* Original FS */
#ifdef EFILESYS
	fat_close ();
#else
	free_map_close ();
#endif
}

/*	name으로 이름이 지어지고 initial_size로 초기화된 파일을 만든다.
	성공하면 true, 실패하면 false를 반환한다.
	이미 같은 이름의 파일이 있거나 
	내부 메모리 할당이 실패하면 실패한다.*/
bool
filesys_create (const char *name, off_t initial_size) {
	disk_sector_t inode_sector = 0;
	struct dir *dir = dir_open_root ();
	bool success = (dir != NULL
			&& free_map_allocate (1, &inode_sector)
			&& inode_create (inode_sector, initial_size)
			&& dir_add (dir, name, inode_sector));
	if (!success && inode_sector != 0)
		free_map_release (inode_sector, 1);
	dir_close (dir);

	return success;
}

/*	name이라고 이름되어있는 파일을 오픈한다.
	성공하면 파일을 반환하고 실패하면 null 포인터를 반환한다.
	name이라 이름지어진 파일이 없거나 
	내부 메모리 할당이 실패하면 실패한다.*/
struct file *
filesys_open (const char *name) {
	struct dir *dir = dir_open_root ();
	struct inode *inode = NULL;

	if (dir != NULL)
		dir_lookup (dir, name, &inode);
	dir_close (dir);

	return file_open (inode);
}


/*	name이라고 이름지어져있는 파일을 삭제한다.
	성공하면 true, 실패하면 false를 반환한다.
	name이라 이름지어진 파일이 없거나
	내부 메모리 할당이 실패하면 실패한다. */
bool
filesys_remove (const char *name) {
	struct dir *dir = dir_open_root ();
	bool success = dir != NULL && dir_remove (dir, name);
	dir_close (dir);

	return success;
}

/* 파일 시스템을 포맷한다. */
static void
do_format (void) {
	printf ("Formatting file system...");

#ifdef EFILESYS
	/* Create FAT and save it to the disk. */
	fat_create ();
	fat_close ();
#else
	free_map_create ();
	if (!dir_create (ROOT_DIR_SECTOR, 16))
		PANIC ("root directory creation failed");
	free_map_close ();
#endif

	printf ("done.\n");
}
