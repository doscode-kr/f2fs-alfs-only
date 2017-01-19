# F2FS extension - ALFS
This is a working repository for integration of ALFS into F2FS. 
ALFS (Application-managed Log-structured File-System) is a filesystem which is modified from F2FS.
Please see details [@fast'16](https://www.usenix.org/node/194457)

### Contribution guidelines ###
* The maintaining rule of this repository follows git flow and GitHub flow together.
* You can learn about them here (A good Korean tutorial!) [@slideshare](http://www.slideshare.net/flyskykr/github-46014813) (Thanks sinsy200)
* The master branch is for maintaining the project. Make a pull request if you are done a piece of job. 

### Environmental setup ###
* Centos7 on linux kernel 4.8.1

### How to set up ###
#### before you get started with ALFS ####
* Get f2fs-alfs tools [@f2fs-alfs-tools](https://github.com/doscode-kr/f2fs-alfs-tools)
* Use mkfs for making the file system 
* Make a f2fs-alfs file system on a device. (i.g., `sudo mkfs.f2fs -s 8 -a 0 -d 1 -l ALFSonNVMe /dev/nvme0n1`)
* 
  #### How to set up ALFS on F2FS ####
* Build file system kernel module and install it using `sudo insmod f2fs.ko`)
* Mount it  (i.g., `sudo mount -t f2fs -o discard /dev/nvme0n1 /media/nvme0n1`)
