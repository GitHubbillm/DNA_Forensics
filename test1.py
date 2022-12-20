# ============================================================
#
# test1.py
#
# This is a quick program to randomly delete a certain number of files
# off of a mounted filesystem. They'll get copied over to the pattern
# directory first so that we can search for them.
#
# Now, if you give us a percentage, we'll kill that percent of the
# file prior to deleting it.
#
# Usage: python3 test1.py <mountpoint> <patterns> [<percent>] [<#files>]
#
# Implementation: Bill Mahoney
#
# ============================================================

import sys
import os
import fnmatch   # Filename matcher
import random
import shutil

# Tell us where the filesystem is mounted. Or else.
if ( len( sys.argv ) < 3 ):
    print( "usage: " + sys.argv[ 0 ] + " <mountpoint> <patterndest> [<percent>] [<number of files>]" )
    sys.exit( 1 )

# Check that the directories exist.
if ( not os.path.isdir( sys.argv[ 1 ] ) or not os.path.isdir( sys.argv[ 2 ] ) ):
    print( "One of the two directories does not seem to exist." )
    sys.exit( 1 )

# If the user provided a percentage, use it, else don't delete
# anything extra.
if ( len( sys.argv ) >= 4 ):
    percent = int( sys.argv[ 3 ] )
else:
    percent = 0

# If there is a number of files, use that, else 100 files.
if ( len( sys.argv ) >= 5 ):
    number_of_files = int( sys.argv[ 4 ] )
else:
    number_of_files = 100

target = sys.argv[ 1 ]
pdir = sys.argv[ 2 ]
pattern = '*jpg'
fileList = []
    
# Walk through directory and make a list of all jpegs.
for dName, sdName, fList in os.walk( target ):
    for fileName in fList:
        if fnmatch.fnmatch( fileName, pattern ):
            fileList.append( os.path.join( dName, fileName ) )

# Now select a set to delete.
# Delete them after the fact or the space will get re-used.

kill_me = []
try:
    for i in range( number_of_files ):
        which = random.randrange( len( fileList ) )
        source = fileList[ which ]
        print( "Copying " + source + " to " + pdir )
        dest = os.path.join( pdir, os.path.basename( source ) )
        shutil.copyfile( source, dest )
        fileList.remove( source )
        kill_me.append( source )
except Exception as inst:
        print( type( inst ) )  # the exception instance
        print( inst.args )     # arguments stored in .args
        print( inst )   

# Yes, you need to sync. If not you will delete the files out of
# the block cache and maybe they never make it to the device. Took a
# while to realize this, and I'm the guy that taught the Operating
# Systems class for a couple decades.

os.system( '/bin/sync' )

# Here we will delete  parts of files. This is a little  odd how I did
# this. If  you say 60% then  I will pick  60% of the sectord  for the
# file and fill them with 60% junk. So the whole blocks are not filled
# with junk unless you say 100%.

if ( percent != 0 ):
    print( "Deleting partial file content, " + str( percent ) + "% of them" )
    # It can just as easily be the same random sector.
    rand_fd = os.open( "/dev/urandom", os.O_RDONLY )
    junk = os.read( rand_fd, 512 )
    os.close( rand_fd )
    for k in kill_me:
        fd = os.open( k, os.O_RDWR )
        stat = os.fstat( fd )
        bytes = stat.st_size
        # This needs to match "searcher.cpp" of course.
        # Maybe I should have used stat.st_blocks?
        sectors = int( ( bytes + 511 ) / 512 )
        killfull = int( ( percent * sectors ) / 100 )
        # print( k + " bytes = " + str( bytes ) + " sectors = " + str( sectors ) + " so " + str( killfull ) )
        # OK we want to not accidentally pick the same sector more than once.
        seclist = list( range( 0, sectors ) )
        for i in range( 0, killfull ):
            nuke = seclist[ random.randrange( len( seclist ) ) ]
            seclist.remove( nuke )
            offset = nuke * 512
            os.lseek( fd, offset, os.SEEK_SET )
            w = os.write( fd, junk[ 0:int( ( percent * 512 ) / 100 ) ] )
            # print( "killed sector " + str( nuke ) + " " + str( w ) )
        os.close( fd )

# Sync up just in case.
os.system( '/bin/sync' )

# Finally we delete the files from the device.
for k in kill_me:
    print( "Deleting original file " + k )
    os.remove( k )

# And one more for good measure.
os.system( '/bin/sync' )

