// ============================================================
//
// DNA inspred slack space searcher.
//
// Usage: ./searcher
//        -d <device>               The name of the device to examine.
//        -p <pattern_dir>          The directory with the patterns (files) to look for.
//        -t <threads>              Number of threads to start on this machine.
//        -c <disk_chunk_size>      Read this many bytes at a time from the device.
//        -f <file_chunk_size>      Read this many bytes at a time from the patterns.
//        -l                        Increases the log level (debugging) by 1 per use.
//
// Implementation: Bill Mahoney
// For:            General purpose experimentation!
// Note:           This will not work (and possibly even segfault) if:
//                 -- <file_chunk_size> (the size of the pattern) is < the size of a machine word
//                 -- <file_chunk_size> (the size of the pattern) is > <disk_chunk_size>
//                 -- the size of the text is < the size of the machine word
//
// Couple other notes. How to know the block size of a drive?
//     blockdev --getbsz /dev/sdb
//     stat -fc '%s' /dev/sdb1
//
// But this says 4096? Really? I know it is not. It's pretty much
// necessary to do everything with the understanding that a filesystem
// has a minimum block size of 512. So a "block" is 512 in all cases.
//
// ============================================================

#include <iostream>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#define POSIX_THREADS 1 // Use threads?

using namespace std;

typedef unsigned long PATTERN_WORD; // On Ubuntu this is 8 bytes
const unsigned int SEC_SIZE = 512;  // Works better than 4K

// Note that some of these are global variables but they are set only
// once based on the command line, so I left them here.
off64_t disk_chunk = 1048576;             // Read this many from the device
off64_t disk_loops = 0;                   // How many disk_chunk's worth are in the image?
off64_t file_chunk = 65536;               // One chunk's worth out of the file we're looking for
unsigned int threads = 8;                 // How many do you want to run?
unsigned int log_level = 0;               // How much information do you want to see?
char *patterns = (char *) "./patterns";   // Default pattern directory
char *device = (char *) "/data/bill_disk_images/FAT1G";

enum status_e {
	       available = 1,
	       needs_data, 
	       needs_cpu,
	       completed
};

const char *status_e[] = {
			  "oops",
			  "available",
			  "needs_data", 
			  "needs_cpu",
			  "completed"
};

// The structure that goes back and forth to the threads
struct search_s {
    enum status_e status;            // What is this one up to now?
    int           fd;                // File descriptor for this file
    char          *filename;         // Will be strdup'd so free it
    unsigned char *disk;             // Points at a chunk of the disk
    unsigned char *buf;              // Points at a chunk of the file
    unsigned char *match;            // The score array for this file (bytes)
    unsigned int  sector_read_count; // How many did we get on the last read?
    unsigned int  current_sector;    // Where are we in the file?
    unsigned int  total_sectors;     // How many sectors total?
    unsigned int  scans;             // How many scans (disk chunks) so far?
    unsigned int  me;                // So that the threads know what to log
    pthread_t     tid;               // From pthread_create
};

bool setup( int ac, char *av[] );
char *next_file( const char *directory );
unsigned int papm_rl( const unsigned char *t, unsigned int n, const unsigned char *p, unsigned int m );
void *scan_disk_blocks( void *params );
void log( unsigned int, const char * format, ... );
void dump_sector( unsigned char *sec );

// ============================================================
//
// Start here - let's go!
//
// It would be much better to split thos whole "main" into parts but
// the whole thing grew larger a wee bit at a time and I just never
// got to it.
//
// ============================================================

int main( int ac, char *av[] )
{

    if ( sizeof( PATTERN_WORD ) != 8 )
    {
        cerr << "Compile / typedef mistake...\n";
        return( 1 );
    }
    
    if ( ! setup( ac, av ) )
	return( 1 );

    // ============================================================
    // OK let's do the easy thing first and make sure we can open the
    // device, since you might need to be "sudo" to do it.
    // ============================================================

    int disk_fd = open( device, O_RDONLY );
    if ( disk_fd < 0 )
    {
        cerr << "Error opening the device " << av[ 1 ] << ".\n";
        if ( geteuid() != 0 )
            cerr << "Maybe you need to be sudo'd? Or does it not exist?\n";
        perror( "open" );
        exit( 2 );
    }

    // ============================================================
    // We need to make sure that the buffer for the disk image does
    // not leave any fractional reads or we may get false positives on
    // the slack space. Do this after any user-defined buffer size.
    // ============================================================

    off64_t actual_image_size = lseek64( disk_fd, (off64_t) 0, SEEK_END );
    if ( actual_image_size < disk_chunk )
    {
        log( 0, "Adjusting disk_chunk setting down to actual size of %llu\n", actual_image_size );
        disk_chunk = actual_image_size;
        disk_loops = 1;
    }
    else
    {
        if ( actual_image_size % disk_chunk != 0 )
        {
            // This is a problem. For now just punt.
            cerr << "The actual image size in bytes is not divisible by " << disk_chunk << "\n";
            close( disk_fd );
            exit( 3 );
        }
        else
        {
            log( 1, "The setting for disk_chunk looks good - %llu\n", disk_chunk );
            disk_loops = actual_image_size / disk_chunk;
        }
    }

    // ============================================================
    // Set up structures and such... The pointer to the disk buffer
    // and file buffer in the search set are set up once and left.
    // ============================================================

    unsigned int  which_disk_buffer = 0;
    unsigned char *disk_buffer[ 2 ];
    disk_buffer[ 0 ] = (unsigned char *) malloc( disk_chunk );
    disk_buffer[ 1 ] = (unsigned char *) malloc( disk_chunk );
    search_s *search_set = (search_s *) malloc( sizeof( search_s ) * threads );
    if ( ( ! disk_buffer[ 0 ] ) ||
         ( ! disk_buffer[ 1 ] ) ||
         ( ! search_set       ) )
    {
        cerr << "malloc failed!?" << endl;
        exit( 1 );
    }

    for( unsigned int i = 0; i < threads; i++ )
    {
	search_set[ i ].status = available;
        search_set[ i ].disk = disk_buffer[ which_disk_buffer ]; // initially 0
        search_set[ i ].buf = (unsigned char *) malloc( file_chunk );
        search_set[ i ].me = i;
    }
    
    // ============================================================
    // OK here we go
    // ============================================================

    log( 1, "Starting up...\n" );

    off64_t alive = 0;
    bool more_files_to_do = true;
    bool keep_going = true;
    while ( keep_going )
    {
        log( 1, "In the main loop...\n" );

        // See notes above - the setting for disk_chunk must be an
        // even divisor of the size of the filesystem image.
        lseek64( disk_fd, (off64_t) 0, SEEK_SET );

        // I was originally just calling read at the top of the loop,
        // and considered switching to aio_read while the threds were
        // running. But the thread execution time seems longer than
        // the I/O delay so I have not done aio yet.

        ssize_t read_count = read( disk_fd, disk_buffer[ 0 ], disk_chunk );
        
        while ( keep_going && read_count == disk_chunk )
        {
            log( 2, "Still working... Disk chunk %u\n", alive++ );
            if ( alive == disk_loops ) alive = 0;

            // ============================================================
            // First, see if anybody is now finished. If so, print the
            // stats and clean up the slot so that it can be
            // reused. Do this first so that we can fill all available
            // slots if we have any.
            // ============================================================
            
            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == completed )
                {
                    unsigned total = 0;
                    for( unsigned int rep = 0; rep < search_set[ i ].total_sectors; rep++ )
                        total += (unsigned) search_set[ i ].match[ rep ];
                    total /= search_set[ i ].total_sectors;
                    cout << search_set[ i ].filename << ": sectors = "
                         << search_set[ i ].total_sectors << " score = ";
                    if ( total == 10 )
                        cout << "*";
                    else
                        cout << total;
                    cout << " by sector = ";
                    for( unsigned int rep = 0; rep < search_set[ i ].total_sectors; rep++ )
                    {
                        char ch = '*';
                        if ( search_set[ i ].match[ rep ] < 10 )
                            ch = search_set[ i ].match[ rep ] + '0';
                        cout << ch;
                    }
                    cout << endl;
                    close( search_set[ i ].fd );
                    search_set[ i ].fd = -1;
                    free( search_set[ i ].filename );
                    free( search_set[ i ].match );
                    search_set[ i ].status = available;
                }

            // ============================================================
            // Next look for available "slots" to assign work to.
            // ============================================================

            for( unsigned int i = 0; i < threads && more_files_to_do; i++ )
                // Is this slot looking for work?
                if ( search_set[ i ].status == available )
                {
                    char *filename = next_file( patterns );
                    if ( filename )
                    {
                        search_set[ i ].fd = open( filename, O_RDONLY );
                        // If the open is OK we'll use this thread
                        if ( search_set[ i ].fd >= 0 )
                        {
                            log( 2, "search_set[ %d ].filename = %s\n", i, filename );
                            // We want to make this LESS than the actual total number of sectors because
                            // the last sector of the file will be partially filled anyhow so not 100% match.
                            search_set[ i ].total_sectors = lseek64( search_set[ i ].fd, 0, SEEK_END ) / SEC_SIZE;
                            lseek64( search_set[ i ].fd, 0, SEEK_SET );
                            search_set[ i ].current_sector = 0;
                            search_set[ i ].filename = strdup( filename );
                            search_set[ i ].match = (unsigned char *) calloc( search_set[ i ].total_sectors, 1 );
                            search_set[ i ].status = needs_data;
                            search_set[ i ].sector_read_count = 0;
                            search_set[ i ].scans = 0;
                        }
                        else
                            // The handling here is not quite right - we ought to try
                            // the next file but in the same slot. Oh well.
                            perror( filename );
                    }
                    else
                    {
                        // There's no more pattern files.
                        more_files_to_do = false;
                        break;
                    }
                }

            // ============================================================
            // Next, load data for any slot that needs it.
            // ============================================================

            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == needs_data )
                {
                    // Like above, don't round the sectors up, truncate the count down. 
                    search_set[ i ].sector_read_count = read( search_set[ i ].fd, search_set[ i ].buf, file_chunk ) / SEC_SIZE;
                    // If there's not a sector's worth left then don't schedule it.
                    // On the other hand, if there IS data we need some CPU time now.
                    search_set[ i ].status = ( search_set[ i ].sector_read_count > 0 ) ? needs_cpu : completed;
                    log( 2, "search_set[ %d ].sector_read_count = %d and status = %s\n", i,
                         search_set[ i ].sector_read_count, status_e[ search_set[ i ].status ] );
                }

            // ============================================================
            // Do the actual scan of this chunk of image with this set of files.
	    // This log is in honor of my 9th grade algebra teacher, Mr. Willard.
	    // He always said this right at the beginning of every class.
            // ============================================================
            
            log( 2, "Talking stopped. Work. To. Be. Done!\n" );

            #if POSIX_THREADS
            // Start them all in parallel
            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == needs_cpu )
                    pthread_create( &search_set[ i ].tid, NULL,
                                    scan_disk_blocks, (void *) &search_set[ i ] );
            // Let's do the I/O while we wait.
            which_disk_buffer = ( which_disk_buffer == 0 ) ? 1 : 0;
            read_count = read( disk_fd, disk_buffer[ which_disk_buffer ], disk_chunk );
            // Then wait for all to finish
            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == needs_cpu )
                {
                    pthread_join( search_set[ i ].tid, NULL );
                    search_set[ i ].scans++;
                }
            #else
            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == needs_cpu )
                {
                    scan_disk_blocks( (void *) &search_set[ i ] );
                    search_set[ i ].scans++;
                }
            which_disk_buffer = ( which_disk_buffer == 0 ) ? 1 : 0;
            read_count = read( disk_fd, disk_buffer[ which_disk_buffer ], disk_chunk );
            #endif

            // Switch everyone over to use the new disk buffer.
            for( unsigned int i = 0; i < threads; i++ )
                search_set[ i ].disk = disk_buffer[ which_disk_buffer ];
            
            log( 2, "One disk scan completed...\n" );

            // ============================================================
            // Slight optimization heuristic. If we have any sets
            // where all the matches are 100% we may as well mark it
            // as completed and free up the slot for somebody else.
            // ============================================================

            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == needs_cpu )
                {
                    search_set[ i ].status = completed;
                    for( unsigned int m = 0; m < search_set[ i ].total_sectors; m++ )
                        if ( search_set[ i ].match[ m ] < 10 )
                        {
                            search_set[ i ].status = needs_cpu;
                            break;
                        }
                }

            // ============================================================
            // See if somebody needs more data. They need data if they
            // have completed an entire disk scan.
            // ============================================================
            
            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status == needs_cpu )
                {
                    if ( search_set[ i ].scans == disk_loops )
                    {
                        search_set[ i ].scans = 0; // Do the disk again
                        search_set[ i ].status = needs_data;
                        search_set[ i ].current_sector += search_set[ i ].sector_read_count;
                    }
                }

            // ============================================================
            // Finally, if we get to the end of a disk chunk scan and
            // every slot is available then we must have finished all
            // the files.
            // ============================================================
            
            keep_going = false;
            for( unsigned int i = 0; i < threads; i++ )
                if ( search_set[ i ].status != available )
                {
                    keep_going = true;
                    break;
                }

            // ============================================================
            // Debugging - print the status
            // ============================================================
            
            log( 2, "search_set[].status = " );
            for( unsigned int i = 0; i < threads; i ++ )
                log( 2, "%s ", status_e[ search_set[ i ].status ] );
            log( 2, "\n" );
        }
    }
    return( 0 );
}

// ============================================================
//
// setup
//
// Here we will process through the command line arg's and set various
// global values for this run of the tool.
//
// ============================================================

bool setup( int ac, char *av[] )
{
    unsigned long temp;
    bool          ok = true;
    
    for( int i = 1; i < ac; i++ )
	if ( av[ i ][ 0 ] == '-' )
	    switch( av[ i ][ 1 ] )
	    {
	        case 'd': // Device
		    if ( av[ i ][ 2 ] )
			device = &av[ i ][ 2 ];
		    else
			device = av[ ++i ];
		    break;

	        case 'p': // Pattern directory
		    if ( av[ i ][ 2 ] )
			patterns = &av[ i ][ 2 ];
		    else
			patterns = av[ ++i ];
		    break;

	        case 't': // Threads
		    if ( av[ i ][ 2 ] )
			temp = strtoul( (const char *) &av[ i ][ 2 ], NULL, 0 );
		    else
			temp = strtoul( (const char *) av[ ++i ], NULL, 0 );
		    threads = (unsigned int) temp;
		    break;

	        case 'c': // disk chunk
		    if ( av[ i ][ 2 ] )
			temp = strtoul( (const char *) &av[ i ][ 2 ], NULL, 0 );
		    else
			temp = strtoul( (const char *) av[ ++i ], NULL, 0 );
		    disk_chunk = (unsigned int) temp;
		    break;

	        case 'f': // file chunk
		    if ( av[ i ][ 2 ] )
			temp = strtoul( (const char *) &av[ i ][ 2 ], NULL, 0 );
		    else
			temp = strtoul( (const char *) av[ ++i ], NULL, 0 );
		    file_chunk = (unsigned int) temp;
		    break;

                case 'l': // log level
                    log_level++;
                    break;
	    }
	else
	    // Something on command line that's not an option
	    ok = false;
    
    if ( disk_chunk % SEC_SIZE )
    {
	cerr << "The disk chunk size must be a multiple of " << SEC_SIZE << "." << endl
	     << "Might I suggest 1048576 a.k.a. 0x100000?" << endl;
	ok = false;
    }
    
    if ( file_chunk % SEC_SIZE )
    {
	cerr << "The file/pattern chunk size must be a multiple of " << SEC_SIZE << "." << endl
	     << "Might I suggest 65536 a.k.a. 0x10000?" << endl;
	ok = false;
    }

    if ( ! ok )
    {
        cerr << "Usage: " << av[ 0 ]
	     << ": [-d <device>] [-p <patterndir>] [-t <threads>] [-c <diskchunk>] [-f <filechunk>]" << endl
	     << "       <device> has the file system" << endl
             << "       <patterndir> is a directory with file patterns" << endl
	     << "       <threads> is the numbe rof threads to start" << endl
	     << "       <diskchunk> is the size of the chunk to read from the drive, multiple of " << SEC_SIZE << endl
	     << "       <filechunk> is the size of the chunk to read for each pattern, multiple of " << SEC_SIZE << endl
	     << "       Defaults: -d" << device << " -p" << patterns << " -t" << threads << " -c" << disk_chunk << " -f" << file_chunk
	     << endl;
        exit( 1 );
    }

    return( ok );
}

// ============================================================
//
// next_file
//
// The first time this is called it sets up to search the indicated
// directory. The first and subsequent calls give the next filename
// from the directory. Note that the filename is set up in a local
// static char array. It will be overwritten on subsequent calls to
// the function. The function will return NULL insteda of the pointer
// when there's no more files.
// 
// ============================================================

char *next_file( const char* directory )
{

    static DIR *dir;
    static char filename[ 1024 ];
    static unsigned int av2_len;
    static struct dirent *nextfile;
    static bool no_more = false;
    char *ret = NULL;

    // Dirty fix. Doing a "closedir" more than once causes a double
    // free.
    if ( no_more )
        return( NULL );
    
    // Happens on the first call.
    if ( ! dir )
    {
        dir = opendir( directory );
        if ( ! dir )
        {
            cerr << "Error opening the directory " << directory << ".\n";
            perror( "opendir" );
            exit( 2 );
        }
        av2_len = strlen( directory );
    }
    
    nextfile = readdir( dir );
    errno = 0; // The global one in <errno.h>
    while ( nextfile )
        if ( nextfile -> d_name[ 0 ] != '.' )
        {
            // Multiple Linux disclaimers about MAX_PATH can be found...
            // Just print a note and try the next filename. 
            if ( av2_len + strlen( nextfile -> d_name ) + 2 >= 1024 )
                cerr << "Filename too long: " << nextfile -> d_name << endl;
            else
            {
                strcpy( filename, directory );
                if ( filename[ av2_len - 1 ] != '/' )
                    strcpy( &filename[ av2_len ], "/" );
                strcat( filename, nextfile -> d_name );
		ret = filename;
                // Get out and return it
                break;
            }
        }
	else
            // Otherwise try the next one
            nextfile = readdir( dir );

    // If we come out with nextfile null we're either done or an error.
    if ( ! nextfile )
    {
        if ( errno != 0 )
        {
            cerr << "Error reading the pattern directory " << directory << ".\n";
            perror( "readdir" );
        }
        closedir( dir );
        no_more = true;
    }

    return( ret );
}

// ============================================================
// Processor Aware Pattern Matching - right to left
//
// t is the text of size n
// p is the pattern of size m
//
// In this algorithm we are not even bothering to attempt any match
// other than one at the "back end" of t. For our uses this is the
// only one we are looking at so there's no need to track the
// "windows" like in the full algorithm from the paper.
//
// One modification is that if the entire block is made up of zeros we
// return 0 to indicate "NO" match at all.
// ============================================================

unsigned int papm_rl( const unsigned char *t, unsigned int n, const unsigned char *p, unsigned int m )
{

    unsigned int in_text = n - sizeof( PATTERN_WORD );
    unsigned int in_pattern = m - sizeof( PATTERN_WORD );
    unsigned int match_count = 0;
    bool all_zero = true;
    
    while ( *( (PATTERN_WORD *) &p[ in_pattern ] ) == *( (PATTERN_WORD *) &t[ in_text ] ) )
    {
        match_count += sizeof( PATTERN_WORD );

        // If it's not zero we'll remember this fact.
        if ( *( (PATTERN_WORD *) &p[ in_pattern ] ) )
            all_zero = false;
        
        // Tail end of a word not divisible by the word size?  This is
	// the case when "in_pattern" becomes zero. We do want to
	// check the very last (first) word of the pattern so if we
	// did and now that was element zero then we are done.
        if ( in_pattern < sizeof( PATTERN_WORD ) )
            break;
        else
        {
            in_pattern -= sizeof( PATTERN_WORD );
            in_text -= sizeof( PATTERN_WORD );
        }
    }

    // Again, this "should not happen" if we are complete blocks.
    if ( in_pattern < sizeof( PATTERN_WORD ) )
        // At most sizeof( PATTERN_WORD ) - 1 iterations.
        while ( in_pattern != 0 )
        {
            --in_pattern; --in_text;
            if ( p[ in_pattern ] == t[ in_text ] )
                match_count++;
            if ( p[ in_pattern ] )
                all_zero = false;
        }
    
    return( all_zero ? 0 : match_count );
}

// ============================================================
//
// scan_disk_blocks
//
// Note that the function technically returns a pointer only so that
// the prototype / signature matches pthread_create.
//
// ============================================================

void *scan_disk_blocks( void *param )
{
    search_s *data = (search_s *) param;

    // This "should not happen"
    if ( data -> sector_read_count == 0 )
    {
	log( 0, "scan_disk_blocks for thread %u has no sectors? sector_read_count = %u?\n",
	     data -> me, data -> sector_read_count );
        data -> status = completed;
    }

    // Which spot will this map to in the "match" array?
    unsigned right_place = data -> current_sector;

    // This will scan all sectors in this collection from the file.
    for( unsigned int block_offset = 0; block_offset < data -> sector_read_count * SEC_SIZE; block_offset += SEC_SIZE, right_place++ )
    {
        // If we already have a 100% match on this block just skip the test.
        if ( data -> match[ right_place ] < 10 )
        {
            for( unsigned int disk_offset = 0; disk_offset < disk_chunk; disk_offset += SEC_SIZE )
            {
                unsigned int result = papm_rl( (const unsigned char *) data -> disk + disk_offset, SEC_SIZE,
                                               (const unsigned char *) data -> buf + block_offset, SEC_SIZE );
                // 10 = 100% match
                //  9 = >90% match
                //  8 = >80% match
                // ...
                // And the highest score wins.
                unsigned int per = ( result * 10 ) / SEC_SIZE;
                if ( per > data -> match[ right_place ] )
                    data -> match[ right_place ] = per;
                
            }
        }
    }
    
    return( NULL );
}

// ============================================================
//
// log
//
// If/when we do this threaded this will be important since it can be
// made to lock.
//
// ============================================================

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void log( unsigned int importance, const char * format, ... )
{
    if ( log_level >= importance )
    {
        char buffer[ 256 ];
        va_list args;
        pthread_mutex_lock( &lock );
        va_start( args, format );
        vsprintf( buffer, format, args );
        cout << buffer << std::flush;
        va_end( args );
        pthread_mutex_unlock( &lock );
    }
}

// ============================================================
//
// For testing.
//
// ============================================================

void dump_sector( unsigned char *sec )
{
    for( unsigned i = 0; i < 32; i++ )
    {
        for( unsigned j = 0; j < 16; j++ )
            // Yeah, I know, it's C++ and not C.
            printf( "%02X ", sec[ i*16 + j ] );
        for( unsigned j = 0; j < 16; j++ )
            if ( isprint( sec[ i*16 + j ] ) )
                putchar( sec[ i*16 + j ] );
            else
                putchar( '.' );
        putchar( '\n' );
    }
}
