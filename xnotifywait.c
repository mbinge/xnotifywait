/*
 * xnotifywait.c
 *
 * Copyright (c) 2013 binge@live.com.
 *
 * Source released under the GNU GENERAL PUBLIC LICENSE (GPL) Version 2.0.
 * See http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt for details.
 *
 * Compile (Mac OS X 10.8.x only)
 *
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "sys/ioctl.h"
#include "sys/types.h"
#include "sys/sysctl.h"
#include "sys/fsevents.h"
#include <pwd.h>
#include <grp.h>

#define PROGNAME "xnotifywait"
#define PROGVERS "1.0"

#define DEV_FSEVENTS     "/dev/fsevents" // the fsevents pseudo-device
#define FSEVENT_BUFSIZ   131072          // buffer for reading from the device
#define EVENT_QUEUE_SIZE 4096            // limited by MAX_KFS_EVENTS

// an event argument
#pragma pack(2)
typedef struct kfs_event_arg {
    u_int16_t  type;         // argument type
    u_int16_t  len;          // size of argument data that follows this field
    union {
        struct vnode *vp;
        char         *str;
        void         *ptr;
        int32_t       int32;
        dev_t         dev;
        ino_t         ino;
        int32_t       mode;
        uid_t         uid;
        gid_t         gid;
        uint64_t      timestamp;
    } data;
} kfs_event_arg_t;
#pragma pack()

#define KFS_NUM_ARGS  FSE_MAX_ARGS

// an event
typedef struct kfs_event {
    int32_t         type; // event type
    pid_t           pid;  // pid of the process that performed the operation
    kfs_event_arg_t args[KFS_NUM_ARGS]; // event arguments
} kfs_event;

// event names
static const char *kfseNames[] = {
    "CREATE",
    "DELETE",
    "FSE_STAT_CHANGED",
    "RENAME",
    "MODIFY",
    "FSE_EXCHANGE",
    "FSE_FINDER_INFO_CHANGED",
    "CREATE",
    "FSE_CHOWN",
    "FSE_XATTR_MODIFIED",
    "FSE_XATTR_REMOVED",
};

// argument names
static const char *kfseArgNames[] = {
    "FSE_ARG_UNKNOWN", "FSE_ARG_VNODE", "FSE_ARG_STRING", "FSE_ARGPATH",
    "FSE_ARG_INT32",   "FSE_ARG_INT64", "FSE_ARG_RAW",    "FSE_ARG_INO",
    "FSE_ARG_UID",     "FSE_ARG_DEV",   "FSE_ARG_MODE",   "FSE_ARG_GID",
    "FSE_ARG_FINFO",
};

// for pretty-printing of vnode types
enum vtype {
    VNON, VREG, VDIR, VBLK, VCHR, VLNK, VSOCK, VFIFO, VBAD, VSTR, VCPLX
};

enum vtype iftovt_tab[] = {
    VNON, VFIFO, VCHR, VNON, VDIR,  VNON, VBLK, VNON,
    VREG, VNON,  VLNK, VNON, VSOCK, VNON, VNON, VBAD,
};

static const char *vtypeNames[] = {
    "VNON",  "VREG",  "VDIR", "VBLK", "VCHR", "VLNK",
    "VSOCK", "VFIFO", "VBAD", "VSTR", "VCPLX",
};
#define VTYPE_MAX (sizeof(vtypeNames)/sizeof(char *))

static char *
get_proc_name(pid_t pid)
{
    size_t        len = sizeof(struct kinfo_proc);
    static int    name[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, 0 };
    static struct kinfo_proc kp;

    name[3] = pid;

    kp.kp_proc.p_comm[0] = '\0';
    if (sysctl((int *)name, sizeof(name)/sizeof(*name), &kp, &len, NULL, 0))
        return "?";

    if (kp.kp_proc.p_comm[0] == '\0')
        return "exited?";

    return kp.kp_proc.p_comm;
}

int compare_str(const void *a,const void *b)
{
	char **first = (char**)a;
	char **second = (char**)b;
	return strcmp(*first,*second);
}

int IsMonitoryRoot(char **dir_list,int dir_count,char *path)
{
	int i=0;
	for(i=0;i<dir_count;i++)
	{
		if(strncmp(*(dir_list+i),path,strlen(*(dir_list+i)))==0)return 1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
    int     fd, clonefd = -1;
    int     i, eoff, off, ret, inMonitor=-1,tgt_inMonitor=-1;
    char    *path=NULL,*tgt_path=NULL,*dir="";

    kfs_event_arg_t *kea;
    struct           fsevent_clone_args fca;
    char             buffer[FSEVENT_BUFSIZ];
    int8_t           event_list[] = { // action to take for each event
                         FSE_REPORT,  // FSE_CREATE_FILE,
                         FSE_REPORT,  // FSE_DELETE,
                         FSE_REPORT,  // FSE_STAT_CHANGED,
                         FSE_REPORT,  // FSE_RENAME,
                         FSE_REPORT,  // FSE_CONTENT_MODIFIED,
                         FSE_REPORT,  // FSE_EXCHANGE,
                         FSE_REPORT,  // FSE_FINDER_INFO_CHANGED,
                         FSE_REPORT,  // FSE_CREATE_DIR,
                         FSE_REPORT,  // FSE_CHOWN,
                         FSE_REPORT,  // FSE_XATTR_MODIFIED,
                         FSE_REPORT,  // FSE_XATTR_REMOVED,
                     };

    if (argc < 2) {
        fprintf(stderr, "%s take dir arguments.\n", PROGNAME);
        exit(1);
    }
    qsort(argv+1,argc-1,sizeof(argv[1]),compare_str);

    if (0 && geteuid() != 0) {
        fprintf(stderr, "You must be root to run %s. Try again using 'sudo'.\n",
                PROGNAME);
        exit(1);
    }

    setbuf(stdout, NULL);

    if ((fd = open(DEV_FSEVENTS, O_RDONLY)) < 0) {
        perror("uopen");
        exit(1);
    }

    fca.event_list = (int8_t *)event_list;
    fca.num_events = sizeof(event_list)/sizeof(int8_t);
    fca.event_queue_depth = EVENT_QUEUE_SIZE;
    fca.fd = &clonefd; 
    if ((ret = ioctl(fd, FSEVENTS_CLONE, (char *)&fca)) < 0) {
        perror("ioctl");
        close(fd);
        exit(1);
    }

    close(fd);
    printf("fsevents device cloned (fd %d)\nxnotifywait ready\n", clonefd);

    if ((ret = ioctl(clonefd, FSEVENTS_WANT_EXTENDED_INFO, NULL)) < 0) {
        perror("ioctl");
        close(clonefd);
        exit(1);
    }

    while (1) 
    { // event processing loop
        if ((ret = read(clonefd, buffer, FSEVENT_BUFSIZ)) < 1)continue;
        off = 0;
        while (off < ret) 
	{ // process one or more events received
            struct kfs_event *kfse = (struct kfs_event *)((char *)buffer + off);
            off += sizeof(int32_t) + sizeof(pid_t); // type + pid
            if (kfse->type == FSE_EVENTS_DROPPED) { // special event
                off += sizeof(u_int16_t); // FSE_ARG_DONE: sizeof(type)
                continue;
            }
            int32_t atype = kfse->type & FSE_TYPE_MASK;
            if ((atype >= FSE_MAX_EVENTS) || (atype < -1)) {// should never happen
                printf("This may be a program bug (type = %d).\n", atype);
                exit(1);
            }

            kea = kfse->args; 
            while (off < ret) 
	    {
                if (kea->type == FSE_ARG_DONE) { // no more arguments
                    off += sizeof(u_int16_t);
                    break;
                }

                eoff = sizeof(kea->type) + sizeof(kea->len) + kea->len;
                off += eoff;

                switch (kea->type) { // handle based on argument type
		case FSE_ARG_STRING: // a string pointer
			switch(kfse->type){
			case FSE_RENAME:
				if(path)tgt_path=(char *)&(kea->data.str);	
			case FSE_CHOWN :
			case FSE_CONTENT_MODIFIED :
			case FSE_CREATE_FILE :
			case FSE_CREATE_DIR :
			case FSE_DELETE :
			case FSE_STAT_CHANGED:
				if(!path)path=(char *)&(kea->data.str);
				break;
			}
			break;
		case FSE_ARG_MODE:
			switch(kfse->type){
			case FSE_RENAME:
                        case FSE_CHOWN :
                        case FSE_CONTENT_MODIFIED :     
                        case FSE_CREATE_FILE :                                          
                        case FSE_CREATE_DIR :                                                                           
                        case FSE_DELETE :                                                                                                               
                        case FSE_STAT_CHANGED:
				dir = kea->data.mode & S_IFDIR ? ":ISDIR":"";
                                break;
                        }
                    	break;
                }
                kea = (kfs_event_arg_t *)((char *)kea + eoff); // next
            } // for each argument
	    if(path)
	    {
		inMonitor = IsMonitoryRoot(argv+1,argc-1,path);
		if(tgt_path)	//RENAME event
		{
			tgt_inMonitor = IsMonitoryRoot(argv+1,argc-1,tgt_path);
			//as inotifywait did 
		//	if(inMonitor==1&&tgt_inMonitor==1)
		//	{
		//		printf("%s,%s MOVED%s\n",path,tgt_path,dir);
		//	}
		//	else
			{
				if(inMonitor==1)printf("%s MOVED_FROM%s\n",path,dir);
				if(tgt_inMonitor==1)printf("%s MOVED_TO%s\n",tgt_path,dir);
			}
		}
		else if(inMonitor==1)
		{
			printf("%s %s%s\n",path,kfseNames[atype],dir);
		}
	    }
	    dir="";path=NULL;tgt_path=NULL;
        } // for each event
    } // forever

    close(clonefd);

    exit(0);
}
