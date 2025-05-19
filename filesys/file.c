#include "filesys/file.h"
#include <debug.h>
#include "filesys/inode.h"
#include "threads/malloc.h"

/* 열린 파일. */
struct file {
	struct inode *inode;        /* 파일의 인덱스 노드 */
	off_t pos;                  /* 현재 파일의 위치. */
	bool deny_write;            /* 쓰기 가능한지 아닌지 여부 */
};

/*
	주어진 인덱스 넘버를 토대로 소유권을 가진 파일은 연다.
	이후 새로운 파일을 리턴한다. 
	만약 할당이 실패하거나 인덱스 노드가 NULL이면 NULL 포인터를 리턴한다. */
struct file *
file_open (struct inode *inode) {
	struct file *file = calloc (1, sizeof *file);
	// 조건 만족하면 파일 구조체에 원소 입력하고 파일 반환
	if (inode != NULL && file != NULL) {
		file->inode = inode;
		file->pos = 0;
		file->deny_write = false;
		return file;
	} else {	// 만족 못하면 NULL 반환환
		inode_close (inode);
		free (file);
		return NULL;
	}
}

/*
	file이 가리키는 파일을 다시 열고
	그 포인터를 반환한다. */
struct file *
file_reopen (struct file *file) {
	return file_open (inode_reopen (file->inode));
}

/*
	속성들을 가진 파일을 복사하고 같은 인덱스 노드에 새로운 파일을 리턴한다.
	실패하면 null 포인터를 리턴한다. */
struct file *
file_duplicate (struct file *file) {
	struct file *nfile = file_open (inode_reopen (file->inode));
	if (nfile) {
		nfile->pos = file->pos;
		if (file->deny_write)
			file_deny_write (nfile);
	}
	return nfile;
}

/* 파일을 닫는다. */
void
file_close (struct file *file) {
	if (file != NULL) {
		file_allow_write (file);
		inode_close (file->inode);
		free (file);
	}
}

/* 파일에서 인덱스 넘버 가져옴. */
struct inode *
file_get_inode (struct file *file) {
	return file->inode;
}

/*
	주어진 파일의 현재 위치에서 시작해서 최대 size 바이트를 버퍼에 읽어들인다.
	실제로 읽은 바이트 수를 반환하고 이 바이트 수는 실제 사이즈보다 짧을 수 있다.
	읽은 바이트 수만큼 file->post를 앞으로 이동시킨다. */
off_t
file_read (struct file *file, void *buffer, off_t size) {
	off_t bytes_read = inode_read_at (file->inode, buffer, size, file->pos);
	file->pos += bytes_read;	// 바이트 읽은 수만큼 file -> pos 이동시킴
	return bytes_read;
}

/*
	주어진 파일의 현재위치에서 시작해서 최대 size 바이트를 버퍼에 읽어들인다.
	실제로 읽은 바이트 수를 반환하고 이 바이트 수는 실제 사이즈보다 짧을 수 있다.
	파일의 현재 위치는 이동시키지 않는다. */
off_t
file_read_at (struct file *file, void *buffer, off_t size, off_t file_ofs) {
	return inode_read_at (file->inode, buffer, size, file_ofs);
}

/*
	현재 파일의 위치부터 시작해서 버퍼에서 SIZE 바이트만큼을 파일에 쓴다.
	실제로 쓴 바이트의 숫자를 반환한다.
	이 바이트 수는 실제 사이즈보다 짧을 수 있다.
	(이 때 일반적으로는 파일 크기를 키우지만 이것은 아직 구현되어있지 않다.)
	바이트를 읽은 수만큼 파일의 위치를 앞으로 이동시킨다. */
 off_t
file_write (struct file *file, const void *buffer, off_t size) {
	off_t bytes_written = inode_write_at (file->inode, buffer, size, file->pos);
	file->pos += bytes_written;
	return bytes_written;
}

/*
	주어진 파일의 file_ofs에서 시작해서 버퍼에서 SIZE 바이트만큼을 파일에 쓴다.
	실제로 쓴 바이트의 숫자를 반환한다.
	이 바이트 수는 실제 사이즈보다 짧을 수 있다.
	(이 때 일반적으로는 파일 크기를 키우지만 이것은 아직 구현되어있지 않다.)
	파일의 현재 위치는 변경되지 않는다. */
off_t
file_write_at (struct file *file, const void *buffer, off_t size,
		off_t file_ofs) {
	return inode_write_at (file->inode, buffer, size, file_ofs);
}

 /*
	파일에 쓰기 권한을 막아서 쓰기 작업을 막는다.
	file_allow_write() 함수가 불러질 때나 파일이 closed 될 때까지 지속한다. */
void
file_deny_write (struct file *file) {
	ASSERT (file != NULL);
	if (!file->deny_write) {
		file->deny_write = true;
		inode_deny_write (file->inode);
	}
}

/*
	파일에 다시 쓰기 기능을 가능하게 한다.
	(같은 인덱스 노드에 다른 파일이 열려있다면 거부 될 수 있다.) */
void
file_allow_write (struct file *file) {
	ASSERT (file != NULL);
	if (file->deny_write) {
		file->deny_write = false;
		inode_allow_write (file->inode);
	}
}

/* 파일의 길이를 리턴한다. */
off_t
file_length (struct file *file) {
	ASSERT (file != NULL);
	return inode_length (file->inode);
}

/*
	주어진 파일의 현재 위치를
	파일의 현재위치에서 NEW_POS 만큼 떨어진 위치로 옮긴다. */
void
file_seek (struct file *file, off_t new_pos) {
	ASSERT (file != NULL);
	ASSERT (new_pos >= 0);
	file->pos = new_pos;
}

/*
	파일의 현재 위치를 반환한다. */
off_t
file_tell (struct file *file) {
	ASSERT (file != NULL);
	return file->pos;
}
