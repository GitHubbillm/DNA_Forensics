# ============================================================
#
# maxout.py
#
# This is a script to fill a device with files until no more will
# fit. Used for the DNA-inspired forensic project. Now, these comments
# were stolen from "fillup.py". The problem is that we don't have
# enough JPEGs to fill anything larger than a gig. So I wrote this
# script to max out the drive with random files of random stuff.
#
# Implementation: Bill Mahoney
#
# ============================================================

import sys
import os
# import fnmatch   # Filename matcher
import random
import shutil    # For copying files

IMAGEDIR='/data/bill_image_data'

# They at least have to give us the device mount point.
if ( len( sys.argv ) != 2 ):
    print( "usage: " + sys.argv[ 0 ] + " <mountpoint>" )
    sys.exit( 1 )

destination = sys.argv[ 1 ]

# Exists?
if ( not os.path.isdir( destination ) ):
    print( "The directory (mountpoint) " + destination + " does not seem to exist." )
    sys.exit( 1 )

# Create random files and write them on the device.

rand_fd = os.open( "/dev/urandom", os.O_RDONLY )
rand_64k = os.read( rand_fd, 65536 )
os.close( rand_fd )

newdir = ''
count = 0
keep_going = True

while keep_going:
    try:
        if ( count % 100 == 0 ):
            newdir = str( int( count / 100 ) ).zfill( 4 )
            maybe = destination + '/' + newdir
            if ( not os.path.isdir( maybe ) ):
                os.mkdir( maybe, 0o777 )
        random_filename = str( random.randrange( 1000000 ) ).zfill( 6 )
        random_filename = random_filename + "_" + str( random.randrange( 1000000 ) ).zfill( 6 )
        dest = os.path.join( destination, newdir, random_filename )
        fd = os.open( dest, os.O_WRONLY | os.O_CREAT, 0o777 )
        # Maximum file on FAT32 is 4G
        if ( random.randrange( 2 ) == 0 ):
            size = random.randrange( 4 * 1024 * 1024 * 1024 )
        else:
            size = random.randrange( 4 * 1024 * 1024 )
        for i in range( int( size / 65536 ) ):
            os.write( fd, rand_64k )
        os.write( fd, rand_64k[ 0 : size % 65536 ] )
        os.close( fd )
        count = count + 1
    except Exception as inst:
        print( type( inst ) )  # the exception instance
        print( inst.args )     # arguments stored in .args
        print( inst )   
        keep_going = False
        
    # Temporary, just make one file
    # keep_going = False
    
os.close( rand_fd )
