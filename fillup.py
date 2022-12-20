# ============================================================
#
# fillup.py
#
# This is a script to fill a device with files until no more will
# fit. Used for the DNA-inspired forensic project.
#
# One issue I bumped into is that there is a limit (even on FAT32) of
# the number of files I can put into the root directory. Not sure why
# but you hit the limit (for a 1G image) at 21844 files. If you move
# some into a sub-directory you can put more in the root, so we just
# create sub-dir's with each 5000 images copied.
#
# Implementation: Bill Mahoney
#
# ============================================================

import sys
import os
import fnmatch   # Filename matcher
import random
import shutil    # For copying files

IMAGEDIR='/data/bill_image_data'

# They at least have to give us the device mount point.
if ( len( sys.argv ) != 2 ):
    print( "usage: " + sys.argv[ 0 ] + " <mountpoint>" )
    sys.exit( 1 )

destination = sys.argv[ 1 ]
pattern = '*jpg'
fileList = []

# Exists?
if ( not os.path.isdir( destination ) ):
    print( "The directory (mountpoint) " + destination + " does not seem to exist." )
    sys.exit( 1 )

# Walk through directory and make a list of all jpegs.
for dName, sdName, fList in os.walk( IMAGEDIR ):
    for fileName in fList:
        if fnmatch.fnmatch( fileName, pattern ):
            fileList.append( os.path.join( dName, fileName ) )

# Now pick files at random and copy them over.
# Keep going until the device is completely full.
newdir = ''
count = 0
keep_going = True
while keep_going:
    try:
        # Select a source file at random
        which = random.randrange( len( fileList ) )
        source = fileList[ which ]
        fileList.remove( source )
        # Check if sub-directory exists and create if not.
        if ( count % 1000 == 0 ):
            # newdir = f'{thou:04d}'
            newdir = str( int( count / 1000 ) ).zfill( 4 )
            maybe = destination + '/' + newdir
            if ( not os.path.isdir( maybe ) ):
                os.mkdir( maybe, 0o777 )
                # print( 'Created ' + maybe )
        dest = os.path.join( destination, newdir, os.path.basename( source ) )
        print( 'Copying ' + source + ' to ' + dest )
        shutil.copyfile( source, dest )
        count = count + 1
    # Most of these exceptions "should not happen"...
    except shutil.SameFileError:
        print("Source and destination are the same file.")
        keep_going = False
    except IsADirectoryError:
        print("Destination is a directory.")
        keep_going = False
    except PermissionError:
        print("Permission denied.")
        keep_going = False
    except:
        # This one is the one we are looking for - out of space.
        print("Error occurred while copying file.")
        keep_going = False
        
