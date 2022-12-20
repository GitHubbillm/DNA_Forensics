############################################################
#
# Makefile for the "DNA" forensics project.
#
# Notes: The variables like M128 are "128 meg". Tools like "mkfs.fat"
# want the number of blocks and in the case of FAT this is in 1024
# byte increments.
#
# Author: Bill M.
#
############################################################

DEVICEDIR = /data/bill_disk_images

# The size is in K, so this is multipled by 1024. Not sure how to find
# the number of clusters other than to just watch it create in verbose
# mode. What we're looking for is the line like this:
# FAT size is 2044 sectors, and provides 261629 clusters.
# But note that this is before a root directory is allocated so the
# minute you put a file on it the capacity is one fewer custer.

# 64M Disk
# SIZE=65536

# 1G Disk
# SIZE=1048576

# 2G
# SIZE=2097152

# 100G
SIZE=104857600

# 512G
BIGSIZE=536870912

all :	scar

############################################################
# Run the tests for the paper.
############################################################

paper :	scar
	/bin/echo ./bill_disk_images/FAT_10_files_deleted
	./scar -d ./bill_disk_images/FAT_10_files_deleted -p ./bill_disk_images/the_deleted_jpegs -t 4
	/bin/echo ./bill_disk_images/FAT_10_files_first_25_pct_overwritten
	./scar -d ./bill_disk_images/FAT_10_files_first_25_pct_overwritten -p ./bill_disk_images/the_deleted_jpegs -t 4
	/bin/echo ./bill_disk_images/FAT_10_files_first_50_pct_overwritten
	./scar -d ./bill_disk_images/FAT_10_files_first_50_pct_overwritten -p ./bill_disk_images/the_deleted_jpegs -t 4
	/bin/echo ./bill_disk_images/FAT_10_files_first_75_pct_overwritten
	./scar -d ./bill_disk_images/FAT_10_files_first_75_pct_overwritten -p ./bill_disk_images/the_deleted_jpegs -t 4
	/bin/echo ./bill_disk_images/FAT_10_files_25_pct_overwritten_at_random
	./scar -d ./bill_disk_images/FAT_10_files_25_pct_overwritten_at_random -p ./bill_disk_images/the_deleted_jpegs -t 4
	/bin/echo ./bill_disk_images/FAT_10_files_50_pct_overwritten_at_random
	./scar -d ./bill_disk_images/FAT_10_files_50_pct_overwritten_at_random -p ./bill_disk_images/the_deleted_jpegs -t 4
	/bin/echo ./bill_disk_images/FAT_10_files_75_pct_overwritten_at_random
	./scar -d ./bill_disk_images/FAT_10_files_75_pct_overwritten_at_random -p ./bill_disk_images/the_deleted_jpegs -t 4

############################################################
# Test number 1: Create a FAT FS and fill it up. Then set aside some
# of the files, and delete them from the image. See if we can setill
# see the files. Trivial.
############################################################

test1 :	FAT
	-sudo umount ./mnt
	sudo mount -t vfat $(DEVICEDIR)/FAT ./mnt -o rw,umask=0000
	/bin/echo Filling...
	python3 fillup.py ./mnt | wc -l
	df ./mnt
	-rm ./patterns/*
	python3 test1.py ./mnt ./patterns $(PERCENT) 10
	sudo umount ./mnt
	time -v ./scar -d /data/bill_disk_images/FAT -p ./patterns -t 24

############################################################
# Test number 2: Same as test 1 but with EXT4 filesystem.
############################################################

test2 :	EXT
	-sudo umount ./mnt
	sudo mount --read-write $(DEVICEDIR)/EXT ./mnt
	sudo chmod 777 ./mnt
	/bin/echo Filling...
	python3 fillup.py ./mnt | wc -l
	df ./mnt
	-rm ./patterns/*
	python3 test1.py ./mnt ./patterns $(PERCENT)
	sudo umount ./mnt
	time -v ./scar -d /data/bill_disk_images/EXT -p ./patterns -t 24

############################################################
# Test number 3: FAT filesystem and we nuke parts of the random files
# before they are deleted.
############################################################

test3 :
	/bin/bash -c 'PERCENT=50 ; export PERCENT ; make test1'

############################################################
# Test number 4: Same as test 3 but for EXT4.
############################################################

test4 :
	/bin/bash -c 'PERCENT=50 ; export PERCENT ; make test2'

############################################################
# Test number 5: Make a much larger filesystem and fill it with random
# files (with random data). Technically this depends on BIGFAT but it
# takes 45 minutes or so to make that filesystem so I left the
# dependency off. What we want to do is delete enough files off of
# BIGFAT that when we run "fillup.py" the files will fit.
#
# For the moment let's try 25% deletion.
############################################################

test5 :
	-rm ./patterns/*
	-sudo umount ./mnt
	sudo mount $(DEVICEDIR)/BIGFAT ./mnt -o rw,umask=0000
	python3 test1.py ./mnt ./patterns 0 100
	sudo umount ./mnt
	time -v ./scar -d /data/bill_disk_images/BIGFAT -p ./patterns -t 24 -c 1073741824

fill5 :	BIGFAT
	sudo mount $(DEVICEDIR)/BIGFAT ./mnt -o rw,umask=0000
	/bin/echo Filling...
	python3 maxout.py ./mnt
	df ./mnt
	sudo umount ./mnt

############################################################
# Make various filesystems. Note that we will nuke the previous copy!
############################################################

FAT :	
	-mkdir $(DEVICEDIR)
	-rm $(DEVICEDIR)/FAT
	/sbin/mkfs.fat -v -F 32 -n 'FAT 32' -C $(DEVICEDIR)/FAT $(SIZE)

EXT :
	-mkdir $(DEVICEDIR)
	-rm $(DEVICEDIR)/EXT
	/sbin/mkfs.ext4 -v -L 'Bill ext4' $(DEVICEDIR)/EXT $(SIZE)

BIGFAT :
	-make umount
	-rm $(DEVICEDIR)/BIGFAT
	/sbin/mkfs.fat -v -F 32 -n 'BIGFAT 32' -C $(DEVICEDIR)/BIGFAT $(BIGSIZE)

############################################################
# Scar program
# Optimized and debug versions.
############################################################

scar :	scar.cpp
	g++ -O2 -Wall -pedantic -o scar scar.cpp -lpthread

debug :	scar.cpp
	g++ -g -Wall -pedantic -o debug scar.cpp -lpthread

############################################################
# Processor aware pattern matching code. Not used any more but was
# used to test the original algorithms.
############################################################

papm :	papm.cpp
	g++ -Wall -pedantic -g -o papm papm.cpp

clean :
	-rm ./scar ./debug ./papm *~
