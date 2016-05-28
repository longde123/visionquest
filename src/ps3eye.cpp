#include "ps3eye.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#if defined WIN32 || defined _WIN32 || defined WINCE
	#include <windows.h>
	#include <algorithm>

	#ifdef __MINGW32__
		void SetThreadName(const char* threadName)
		{
			// Not sure how to implement this on mingw
		}
	#else
		const DWORD MS_VC_EXCEPTION=0x406D1388;

		#pragma pack(push,8)
		typedef struct tagTHREADNAME_INFO
		{
			DWORD dwType;		// Must be 0x1000.
			LPCSTR szName;		// Pointer to name (in user addr space).
			DWORD dwThreadID;	// Thread ID (-1=caller thread).
			DWORD dwFlags;		// Reserved for future use, must be zero.
		} THREADNAME_INFO;
		#pragma pack(pop)

		void SetThreadName(uint32_t thread_id, const char* thread_name)
		{
			THREADNAME_INFO info;
			info.dwType = 0x1000;
			info.szName = thread_name;
			info.dwThreadID = thread_id;
			info.dwFlags = 0;

			__try
			{
				RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		void SetThreadName(const char* thread_name)
		{
			SetThreadName(GetCurrentThreadId(), thread_name);
		}
	#endif

#else
	#include <sys/time.h>
	#include <time.h>
	#if defined __MACH__ && defined __APPLE__
		#include <mach/mach.h>
		#include <mach/mach_time.h>
	#endif

	void SetThreadName(const char* threadName)
	{
		// Not sure how to implement this on linux/osx, so left empty...
	}
#endif

namespace ps3eye {

#define TRANSFER_SIZE		16384
#define NUM_TRANSFERS		8

#define OV534_REG_ADDRESS	0xf1	/* sensor address */
#define OV534_REG_SUBADDR	0xf2
#define OV534_REG_WRITE		0xf3
#define OV534_REG_READ		0xf4
#define OV534_REG_OPERATION	0xf5
#define OV534_REG_STATUS	0xf6

#define OV534_OP_WRITE_3	0x37
#define OV534_OP_WRITE_2	0x33
#define OV534_OP_READ_2		0xf9

#define CTRL_TIMEOUT 500
#define VGA	 0
#define QVGA 1

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(_A) (sizeof(_A) / sizeof((_A)[0]))
#endif

static const uint8_t ov534_reg_initdata[][2] = {
	{ 0xe7, 0x3a },

	{ OV534_REG_ADDRESS, 0x42 }, /* select OV772x sensor */

	{ 0xc2, 0x0c },
	{ 0x88, 0xf8 },
	{ 0xc3, 0x69 },
	{ 0x89, 0xff },
	{ 0x76, 0x03 },
	{ 0x92, 0x01 },
	{ 0x93, 0x18 },
	{ 0x94, 0x10 },
	{ 0x95, 0x10 },
	{ 0xe2, 0x00 },
	{ 0xe7, 0x3e },

	{ 0x96, 0x00 },

	{ 0x97, 0x20 },
	{ 0x97, 0x20 },
	{ 0x97, 0x20 },
	{ 0x97, 0x0a },
	{ 0x97, 0x3f },
	{ 0x97, 0x4a },
	{ 0x97, 0x20 },
	{ 0x97, 0x15 },
	{ 0x97, 0x0b },

	{ 0x8e, 0x40 },
	{ 0x1f, 0x81 },
	{ 0x34, 0x05 },
	{ 0xe3, 0x04 },
	{ 0x88, 0x00 },
	{ 0x89, 0x00 },
	{ 0x76, 0x00 },
	{ 0xe7, 0x2e },
	{ 0x31, 0xf9 },
	{ 0x25, 0x42 },
	{ 0x21, 0xf0 },

	{ 0x1c, 0x00 },
	{ 0x1d, 0x40 },
	{ 0x1d, 0x02 }, /* payload size 0x0200 * 4 = 2048 bytes */
	{ 0x1d, 0x00 }, /* payload size */

// -------------

//	{ 0x1d, 0x01 },/* frame size */		// kwasy
//	{ 0x1d, 0x4b },/* frame size */
//	{ 0x1d, 0x00 }, /* frame size */


//	{ 0x1d, 0x02 },/* frame size */		// macam
//	{ 0x1d, 0x57 },/* frame size */
//	{ 0x1d, 0xff }, /* frame size */

	{ 0x1d, 0x02 },/* frame size */		// jfrancois / linuxtv.org/hg/v4l-dvb
	{ 0x1d, 0x58 },/* frame size */
	{ 0x1d, 0x00 }, /* frame size */

// ---------

	{ 0x1c, 0x0a },
	{ 0x1d, 0x08 }, /* turn on UVC header */
	{ 0x1d, 0x0e }, /* .. */

	{ 0x8d, 0x1c },
	{ 0x8e, 0x80 },
	{ 0xe5, 0x04 },

// ----------------
//	{ 0xc0, 0x28 },//	kwasy / macam
//	{ 0xc1, 0x1e },//

	{ 0xc0, 0x50 },		// jfrancois
	{ 0xc1, 0x3c },
	{ 0xc2, 0x0c }, 


	
};

static const uint8_t ov772x_reg_initdata[][2] = {

	{0x12, 0x80 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },
	{0x11, 0x01 },

	{0x3d, 0x03 },
	{0x17, 0x26 },
	{0x18, 0xa0 },
	{0x19, 0x07 },
	{0x1a, 0xf0 },
	{0x32, 0x00 },
	{0x29, 0xa0 },
	{0x2c, 0xf0 },
	{0x65, 0x20 },
	{0x11, 0x01 },
	{0x42, 0x7f },
	{0x63, 0xAA }, 	// AWB
	{0x64, 0xff },
	{0x66, 0x00 },
	{0x13, 0xf0 },	// COM8  - jfrancois 0xf0	orig x0f7
	{0x0d, 0x41 },
	{0x0f, 0xc5 },
	{0x14, 0x11 },

	{0x22, 0x7f },
	{0x23, 0x03 },
	{0x24, 0x40 },
	{0x25, 0x30 },
	{0x26, 0xa1 },
	{0x2a, 0x00 },
	{0x2b, 0x00 }, 
	{0x6b, 0xaa },
	{0x13, 0xff },	// COM8 - jfrancois 0xff orig 0xf7

	{0x90, 0x05 },
	{0x91, 0x01 },
	{0x92, 0x03 },
	{0x93, 0x00 },
	{0x94, 0x60 },
	{0x95, 0x3c },
	{0x96, 0x24 },
	{0x97, 0x1e },
	{0x98, 0x62 },
	{0x99, 0x80 },
	{0x9a, 0x1e },
	{0x9b, 0x08 },
	{0x9c, 0x20 },
	{0x9e, 0x81 },

	{0xa6, 0x04 },
	{0x7e, 0x0c },
	{0x7f, 0x16 },
	{0x80, 0x2a },
	{0x81, 0x4e },
    {0x82, 0x61 },
	{0x83, 0x6f },
	{0x84, 0x7b },
	{0x85, 0x86 },
	{0x86, 0x8e },
	{0x87, 0x97 },
	{0x88, 0xa4 },
	{0x89, 0xaf },
	{0x8a, 0xc5 },
	{0x8b, 0xd7 },
	{0x8c, 0xe8 },
	{0x8d, 0x20 },

	{0x0c, 0x90 },

	{0x2b, 0x00 }, 
	{0x22, 0x7f },
	{0x23, 0x03 },
	{0x11, 0x01 },
	{0x0c, 0xd0 },
	{0x64, 0xff },
	{0x0d, 0x41 },

	{0x14, 0x41 },
	{0x0e, 0xcd },
	{0xac, 0xbf },
	{0x8e, 0x00 },	// De-noise threshold - jfrancois 0x00 - orig 0x04
	{0x0c, 0xd0 }

};

static const uint8_t bridge_start_vga[][2] = {
	{0x1c, 0x00},
	{0x1d, 0x40},
	{0x1d, 0x02},
	{0x1d, 0x00},
	{0x1d, 0x02},
	{0x1d, 0x58},
	{0x1d, 0x00},
	{0xc0, 0x50},
	{0xc1, 0x3c},
};
static const uint8_t sensor_start_vga[][2] = {
	{0x12, 0x00},
	{0x17, 0x26},
	{0x18, 0xa0},
	{0x19, 0x07},
	{0x1a, 0xf0},
	{0x29, 0xa0},
	{0x2c, 0xf0},
	{0x65, 0x20},
};
static const uint8_t bridge_start_qvga[][2] = {
	{0x1c, 0x00},
	{0x1d, 0x40},
	{0x1d, 0x02},
	{0x1d, 0x00},
	{0x1d, 0x01},
	{0x1d, 0x4b},
	{0x1d, 0x00},
	{0xc0, 0x28},
	{0xc1, 0x1e},
};
static const uint8_t sensor_start_qvga[][2] = {
	{0x12, 0x40},
	{0x17, 0x3f},
	{0x18, 0x50},
	{0x19, 0x03},
	{0x1a, 0x78},
	{0x29, 0x50},
	{0x2c, 0x78},
	{0x65, 0x2f},
};

/* Values for bmHeaderInfo (Video and Still Image Payload Headers, 2.4.3.3) */
#define UVC_STREAM_EOH	(1 << 7)
#define UVC_STREAM_ERR	(1 << 6)
#define UVC_STREAM_STI	(1 << 5)
#define UVC_STREAM_RES	(1 << 4)
#define UVC_STREAM_SCR	(1 << 3)
#define UVC_STREAM_PTS	(1 << 2)
#define UVC_STREAM_EOF	(1 << 1)
#define UVC_STREAM_FID	(1 << 0)

/* packet types when moving from iso buf to frame buf */
enum gspca_packet_type {
    DISCARD_PACKET,
    FIRST_PACKET,
    INTER_PACKET,
    LAST_PACKET
};

/*
 * look for an input transfer endpoint in an alternate setting
 * libusb_endpoint_descriptor
 */
static uint8_t find_ep(struct libusb_device *device)
{
	const struct libusb_interface_descriptor *altsetting = NULL;
    const struct libusb_endpoint_descriptor *ep;
	struct libusb_config_descriptor *config = NULL;
    int i;
    uint8_t ep_addr = 0;

    libusb_get_active_config_descriptor(device, &config);

    if (!config) return 0;

    for (i = 0; i < config->bNumInterfaces; i++) {
        altsetting = config->interface[i].altsetting;
        if (altsetting[0].bInterfaceNumber == 0) {
            break;
        }
    }

    for (i = 0; i < altsetting->bNumEndpoints; i++) {
        ep = &altsetting->endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK 
            && ep->wMaxPacketSize != 0) 
        {
            ep_addr = ep->bEndpointAddress;
            break;
        }
    }

    libusb_free_config_descriptor(config);

    return ep_addr;
}

const uint16_t PS3EYECam::VENDOR_ID = 0x1415;
const uint16_t PS3EYECam::PRODUCT_ID = 0x2000;

class USBMgr
{
 public:
    USBMgr();
	 ~USBMgr();

	static std::shared_ptr<USBMgr>  instance();
    int listDevices(std::vector<PS3EYECam::PS3EYERef>& list);
	void cameraStarted();
	void cameraStopped();

    static std::shared_ptr<USBMgr>  sInstance;
    static int                      sTotalDevices;

 private:   
    libusb_context*					usb_context;
	std::thread						update_thread;
	std::atomic_bool				exit_signaled;
	std::atomic_int					active_camera_count;

    USBMgr(const USBMgr&);
    void operator=(const USBMgr&);

	void startTransferThread();
	void stopTransferThread();
	void transferThreadFunc();
};

std::shared_ptr<USBMgr> USBMgr::sInstance;
int                     USBMgr::sTotalDevices = 0;

USBMgr::USBMgr() :
	exit_signaled({ false }),
	active_camera_count({ 0 })
{
    libusb_init(&usb_context);
    libusb_set_debug(usb_context, 1);
}

USBMgr::~USBMgr()
{
    debug("USBMgr destructor\n");
    libusb_exit(usb_context);
}

std::shared_ptr<USBMgr> USBMgr::instance()
{
    if( !sInstance ) {
        sInstance = std::shared_ptr<USBMgr>( new USBMgr );
    }
    return sInstance;
}

void USBMgr::cameraStarted()
{
	if (active_camera_count++ == 0)
		startTransferThread();
}

void USBMgr::cameraStopped()
{
	if (--active_camera_count == 0)
		stopTransferThread();
}

void USBMgr::startTransferThread()
{
	update_thread = std::thread(&USBMgr::transferThreadFunc, this);
}

void USBMgr::stopTransferThread()
{
	exit_signaled = true;
	update_thread.join();
	// Reset the exit signal flag.
	// If we don't and we call startTransferThread() again, transferThreadFunc will exit immediately.
	exit_signaled = false;    
}

void USBMgr::transferThreadFunc()
{
	SetThreadName("PS3EyeDriver Transfer Thread");

	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = 50 * 1000; // ms

	while (!exit_signaled)
	{
#ifdef _WIN32
		libusb_handle_events_timeout_completed(usb_context, &tv, NULL);
#else
        //TODO:shenberg update libusb version on mac?
        libusb_handle_events(usb_context);
#endif
	}
}

int USBMgr::listDevices( std::vector<PS3EYECam::PS3EYERef>& list )
{
	libusb_device *dev;
	libusb_device **devs;
	libusb_device_handle *devhandle;
    int i = 0;
    int cnt;

    cnt = (int)libusb_get_device_list(usb_context, &devs);

	if (cnt < 0) {
		debug("Error Device scan\n");
	}

    cnt = 0;
    while ((dev = devs[i++]) != NULL) 
	{
		struct libusb_device_descriptor desc;
		libusb_get_device_descriptor(dev, &desc);
		if (desc.idVendor == PS3EYECam::VENDOR_ID && desc.idProduct == PS3EYECam::PRODUCT_ID)
		{
			int err = libusb_open(dev, &devhandle);
			if (err == 0)
			{
				libusb_close(devhandle);
				list.push_back( PS3EYECam::PS3EYERef( new PS3EYECam(dev) ) );
				libusb_ref_device(dev);
				cnt++;
			}
		}
	}

	libusb_free_device_list(devs, 1);

    return cnt;
}

static void LIBUSB_CALL transfer_completed_callback(struct libusb_transfer *xfr);

class FrameQueue
{
public:
	FrameQueue(uint32_t frame_size) :
		frame_size			(frame_size),
		num_frames			(2),
		frame_buffer		((uint8_t*)malloc(frame_size * num_frames)),
		head				(0),
		tail				(0),
		available			(0)
	{
	}

	~FrameQueue()
	{
		free(frame_buffer);
	}

	uint8_t* GetFrameBufferStart()
	{
		return frame_buffer;
	}

	uint8_t* Enqueue()
	{
		uint8_t* new_frame = NULL;

		std::lock_guard<std::mutex> lock(mutex);

		// Unlike traditional producer/consumer, we don't block the producer if the buffer is full (ie. the consumer is not reading data fast enough).
		// Instead, if the buffer is full, we simply return the current frame pointer, causing the producer to overwrite the previous frame.
		// This allows performance to degrade gracefully: if the consumer is not fast enough (< Camera FPS), it will miss frames, but if it is fast enough (>= Camera FPS), it will see everything.
		//
		// Note that because the the producer is writing directly to the ring buffer, we can only ever be a maximum of num_frames-1 ahead of the consumer, 
		// otherwise the producer could overwrite the frame the consumer is currently reading (in case of a slow consumer)
		if (available >= num_frames - 1)
		{
			return frame_buffer + head * frame_size;
		}

		// Note: we don't need to copy any data to the buffer since the USB packets are directly written to the frame buffer.
		// We just need to update head and available count to signal to the consumer that a new frame is available
		head = (head + 1) % num_frames;
		available++;

		// Determine the next frame pointer that the producer should write to
		new_frame = frame_buffer + head * frame_size;

		// Signal consumer that data became available
		empty_condition.notify_one();

		return new_frame;
	}

	uint8_t* Dequeue()
	{
		uint8_t* new_frame = (uint8_t*)malloc(frame_size);
		
		std::unique_lock<std::mutex> lock(mutex);

		// If there is no data in the buffer, wait until data becomes available
		empty_condition.wait(lock, [this] () { return available != 0; });

		// Copy from internal buffer
		uint8_t* source = frame_buffer + frame_size * tail;
		memcpy(new_frame, source, frame_size);

		// Update tail and available count
		tail = (tail + 1) % num_frames;
		available--;

		return new_frame;
	}

private:
	uint32_t				frame_size;
	uint32_t				num_frames;

	uint8_t*				frame_buffer;
	uint32_t				head;
	uint32_t				tail;
	uint32_t				available;

	std::mutex				mutex;
	std::condition_variable	empty_condition;
};

// URBDesc

class URBDesc
{
public:
	URBDesc() : 
		num_active_transfers			(0),
		last_packet_type		(DISCARD_PACKET), 
		last_pts				(0), 
		last_fid				(0), 
		transfer_buffer			(NULL),
		cur_frame_start			(NULL),
		cur_frame_data_len		(0),
		frame_size				(0),
		frame_queue				(NULL)
	{
	}

	~URBDesc()
	{
		debug("URBDesc destructor\n");
		close_transfers();
	}

	bool start_transfers(libusb_device_handle *handle, uint32_t curr_frame_size)
	{
		// Initialize the frame queue
        frame_size = curr_frame_size;
		frame_queue = new FrameQueue(frame_size);

		// Initialize the current frame pointer to the start of the buffer; it will be updated as frames are completed and pushed onto the frame queue
		cur_frame_start = frame_queue->GetFrameBufferStart();
		cur_frame_data_len = 0;

		// Find the bulk transfer endpoint
		uint8_t bulk_endpoint = find_ep(libusb_get_device(handle));
		libusb_clear_halt(handle, bulk_endpoint);

		// Allocate the transfer buffer
		transfer_buffer = (uint8_t*)malloc(TRANSFER_SIZE * NUM_TRANSFERS);
		memset(transfer_buffer, 0, TRANSFER_SIZE * NUM_TRANSFERS);

		int res = 0;
		for (int index = 0; index < NUM_TRANSFERS; ++index)
		{
			// Create & submit the transfer
			xfr[index] = libusb_alloc_transfer(0);
			libusb_fill_bulk_transfer(xfr[index], handle, bulk_endpoint, transfer_buffer + index * TRANSFER_SIZE, TRANSFER_SIZE, transfer_completed_callback, reinterpret_cast<void*>(this), 0);

			res |= libusb_submit_transfer(xfr[index]);
			
			num_active_transfers++;
		}

		last_pts = 0;
		last_fid = 0;

		USBMgr::instance()->cameraStarted();

		return res == 0;
	}

	void close_transfers()
	{
		std::unique_lock<std::mutex> lock(num_active_transfers_mutex);
		if (num_active_transfers == 0)
			return;

		// Cancel any pending transfers
		for (int index = 0; index < NUM_TRANSFERS; ++index)
		{
			libusb_cancel_transfer(xfr[index]);
		}

		// Wait for cancelation to finish
		num_active_transfers_condition.wait(lock, [this]() { return num_active_transfers == 0; });

		USBMgr::instance()->cameraStopped();

		free(transfer_buffer);
		transfer_buffer = NULL;

		delete frame_queue;
		frame_queue = NULL;
	}

	void transfer_canceled()
	{
		std::lock_guard<std::mutex> lock(num_active_transfers_mutex);
		--num_active_transfers;
		num_active_transfers_condition.notify_one();
	}

	void frame_add(enum gspca_packet_type packet_type, const uint8_t *data, int len)
	{
	    if (packet_type == FIRST_PACKET) 
	    {
            cur_frame_data_len = 0;
	    } 
	    else
	    {
            switch(last_packet_type)  // ignore warning.
            {
                case DISCARD_PACKET:
                    if (packet_type == LAST_PACKET) {
                        last_packet_type = packet_type;
                        cur_frame_data_len = 0;
                    }
                    return;
                case LAST_PACKET:
                    return;
                default:
                    break;
            }
	    }

	    /* append the packet to the frame buffer */
	    if (len > 0)
        {
            if(cur_frame_data_len + len > frame_size)
            {
                packet_type = DISCARD_PACKET;
                cur_frame_data_len = 0;
            } else {
                memcpy(cur_frame_start+cur_frame_data_len, data, len);
                cur_frame_data_len += len;
            }
	    }

	    last_packet_type = packet_type;

	    if (packet_type == LAST_PACKET) {        
			cur_frame_data_len = 0;
			cur_frame_start = frame_queue->Enqueue();
	        //debug("frame completed %d\n", frame_complete_ind);
	    }
	}

	void pkt_scan(uint8_t *data, int len)
	{
	    uint32_t this_pts;
	    uint16_t this_fid;
	    int remaining_len = len;
	    int payload_len;

	    payload_len = 2048; // bulk type
	    do {
			len = (std::min)(remaining_len, payload_len);

	        /* Payloads are prefixed with a UVC-style header.  We
	           consider a frame to start when the FID toggles, or the PTS
	           changes.  A frame ends when EOF is set, and we've received
	           the correct number of bytes. */

	        /* Verify UVC header.  Header length is always 12 */
	        if (data[0] != 12 || len < 12) {
	            debug("bad header\n");
	            goto discard;
	        }

	        /* Check errors */
	        if (data[1] & UVC_STREAM_ERR) {
	            debug("payload error\n");
	            goto discard;
	        }

	        /* Extract PTS and FID */
	        if (!(data[1] & UVC_STREAM_PTS)) {
	            debug("PTS not present\n");
	            goto discard;
	        }

	        this_pts = (data[5] << 24) | (data[4] << 16) | (data[3] << 8) | data[2];
	        this_fid = (data[1] & UVC_STREAM_FID) ? 1 : 0;

	        /* If PTS or FID has changed, start a new frame. */
	        if (this_pts != last_pts || this_fid != last_fid) {
	            if (last_packet_type == INTER_PACKET)
	            {
	                frame_add(LAST_PACKET, NULL, 0);
	            }
	            last_pts = this_pts;
	            last_fid = this_fid;
	            frame_add(FIRST_PACKET, data + 12, len - 12);
	        } /* If this packet is marked as EOF, end the frame */
	        else if (data[1] & UVC_STREAM_EOF) 
	        {
	            last_pts = 0;
                if(cur_frame_data_len + len - 12 != frame_size)
                {
                    goto discard;
                }
	            frame_add(LAST_PACKET, data + 12, len - 12);
	        } else {
	            /* Add the data from this payload */
	            frame_add(INTER_PACKET, data + 12, len - 12);
	        }


	        /* Done this payload */
	        goto scan_next;

	discard:
	        /* Discard data until a new frame starts. */
	        frame_add(DISCARD_PACKET, NULL, 0);
	scan_next:
	        remaining_len -= len;
	        data += len;
	    } while (remaining_len > 0);
	}

	uint8_t					num_active_transfers;
	std::mutex				num_active_transfers_mutex;
	std::condition_variable	num_active_transfers_condition;

	enum gspca_packet_type	last_packet_type;
	uint32_t				last_pts;
	uint16_t				last_fid;
	libusb_transfer*		xfr[NUM_TRANSFERS];

	uint8_t*				transfer_buffer;
    uint8_t*				cur_frame_start;
	uint32_t				cur_frame_data_len;
	uint32_t				frame_size;
	FrameQueue*				frame_queue;
};

static void LIBUSB_CALL transfer_completed_callback(struct libusb_transfer *xfr)
{
    URBDesc *urb = reinterpret_cast<URBDesc*>(xfr->user_data);
    enum libusb_transfer_status status = xfr->status;

    if (status != LIBUSB_TRANSFER_COMPLETED) 
    {
        debug("transfer status %d\n", status);

        libusb_free_transfer(xfr);
		urb->transfer_canceled();
        
        if(status != LIBUSB_TRANSFER_CANCELLED)
        {
            urb->close_transfers();
        }
        return;
    }

    //debug("length:%u, actual_length:%u\n", xfr->length, xfr->actual_length);

    urb->pkt_scan(xfr->buffer, xfr->actual_length);

    if (libusb_submit_transfer(xfr) < 0) {
        debug("error re-submitting URB\n");
        urb->close_transfers();
    }
}

// PS3EYECam

bool PS3EYECam::devicesEnumerated = false;
std::vector<PS3EYECam::PS3EYERef> PS3EYECam::devices;

const std::vector<PS3EYECam::PS3EYERef>& PS3EYECam::getDevices( bool forceRefresh )
{
    if( devicesEnumerated && ( ! forceRefresh ) )
        return devices;

    devices.clear();

    USBMgr::instance()->sTotalDevices = USBMgr::instance()->listDevices(devices);

    devicesEnumerated = true;
    return devices;
}

PS3EYECam::PS3EYECam(libusb_device *device)
{
	// default controls
	autogain = false;
	gain = 20;
	exposure = 120;
	sharpness = 0;
	hue = 143;
	awb = false;
	brightness = 20;
	contrast =  37;
	blueblc = 128;
	redblc = 128;
	greenblc = 128;
    flip_h = false;
    flip_v = false;

	usb_buf = NULL;
	handle_ = NULL;

	is_streaming = false;

	device_ = device;
	mgrPtr = USBMgr::instance();
	urb = std::shared_ptr<URBDesc>( new URBDesc() );
}

PS3EYECam::~PS3EYECam()
{
	stop();
	release();
}

void PS3EYECam::release()
{
	if(handle_ != NULL) 
		close_usb();
	if(usb_buf) free(usb_buf);
//#ifdef _WIN32
//	if (mutexIpc != NULL) {
//		CloseHandle(mutexIpc);
//	}
//#endif
}

bool PS3EYECam::init(uint32_t width, uint32_t height, uint8_t desiredFrameRate)
{
	uint16_t sensor_id;

	// open usb device so we can setup and go
	if(handle_ == NULL) 
	{
		if( !open_usb() )
		{
			return false;
		}
	}

//#ifdef _WIN32
//	// try to aqcuire usb mutex
//	WCHAR buffer[255] = { 0 };
//	wsprintf(buffer, L"Global\PSEYE_%d_%d", libusb_get_bus_number(device_), libusb_get_device_address(device_));
//	debug("attempting to get mutex %S\n", buffer);
//	mutexIpc = CreateMutex(NULL, TRUE, buffer);
//	if (mutexIpc == NULL || (GetLastError() == ERROR_ALREADY_EXISTS)) {
//		debug("failed to get mutex :(\n");
//		if (mutexIpc) {
//			CloseHandle(mutexIpc);
//		}
//		return false;
//	}
//#endif
	//
	if(usb_buf == NULL)
		usb_buf = (uint8_t*)malloc(64);

	// find best cam mode
	if((width == 0 && height == 0) || width > 320 || height > 240)
	{
		frame_width = 640;
		frame_height = 480;
	} else {
		frame_width = 320;
		frame_height = 240;
	}
	frame_rate = ov534_set_frame_rate(desiredFrameRate, true);
    frame_stride = frame_width * 2;
	//

	/* reset bridge */
	ov534_reg_write(0xe7, 0x3a);
	ov534_reg_write(0xe0, 0x08);

#ifdef _MSC_VER
	Sleep(100);
#else
    nanosleep((const struct timespec[]){{0, 100000000}}, NULL);
#endif

	/* initialize the sensor address */
	ov534_reg_write(OV534_REG_ADDRESS, 0x42);

	/* reset sensor */
	sccb_reg_write(0x12, 0x80);
#ifdef _MSC_VER
	Sleep(10);
#else    
    nanosleep((const struct timespec[]){{0, 10000000}}, NULL);
#endif

	/* probe the sensor */
	sccb_reg_read(0x0a);
	sensor_id = sccb_reg_read(0x0a) << 8;
	sccb_reg_read(0x0b);
	sensor_id |= sccb_reg_read(0x0b);
	debug("Sensor ID: %04x\n", sensor_id);

	/* initialize */
	reg_w_array(ov534_reg_initdata, ARRAY_SIZE(ov534_reg_initdata));
	ov534_set_led(1);
	sccb_w_array(ov772x_reg_initdata, ARRAY_SIZE(ov772x_reg_initdata));
	ov534_reg_write(0xe0, 0x09);
	ov534_set_led(0);

	return true;
}

void PS3EYECam::start()
{
    if(is_streaming) return;
    
	if (frame_width == 320) {	/* 320x240 */
		reg_w_array(bridge_start_qvga, ARRAY_SIZE(bridge_start_qvga));
		sccb_w_array(sensor_start_qvga, ARRAY_SIZE(sensor_start_qvga));
	} else {		/* 640x480 */
		reg_w_array(bridge_start_vga, ARRAY_SIZE(bridge_start_vga));
		sccb_w_array(sensor_start_vga, ARRAY_SIZE(sensor_start_vga));
	}

	ov534_set_frame_rate(frame_rate);

	setAutogain(autogain);
	setAutoWhiteBalance(awb);
	setGain(gain);
	setHue(hue);
	setExposure(exposure);
	setBrightness(brightness);
	setContrast(contrast);
	setSharpness(sharpness);
	setRedBalance(redblc);
	setBlueBalance(blueblc);
	setGreenBalance(greenblc);
    setFlip(flip_h, flip_v);

	ov534_set_led(1);
	ov534_reg_write(0xe0, 0x00); // start stream

	// init and start urb
	urb->start_transfers(handle_, frame_stride*frame_height);
    is_streaming = true;
}

void PS3EYECam::stop()
{
	std::this_thread::sleep_for(std::chrono::milliseconds(450)); //TODO: if we move between usb cameras quickly we crash. need to understand why
    if(!is_streaming) return;

	/* stop streaming data */
	ov534_reg_write(0xe0, 0x09);
	ov534_set_led(0);
    
	// close urb
	urb->close_transfers();

    is_streaming = false;
}

uint8_t* PS3EYECam::getFrame()
{
	return urb->frame_queue->Dequeue();
}

bool PS3EYECam::open_usb()
{
	// open, set first config and claim interface
	int res = libusb_open(device_, &handle_);
	if(res != 0) {
		debug("device open error: %d\n", res);
		return false;
	}

	//libusb_set_configuration(handle_, 0);

	res = libusb_claim_interface(handle_, 0);
	if(res != 0) {
		debug("device claim interface error: %d\n", res);
		return false;
	}

	return true;
}

void PS3EYECam::close_usb()
{
	debug("closing device\n");
	libusb_release_interface(handle_, 0);
	libusb_close(handle_);
	libusb_unref_device(device_);
	handle_ = NULL;
	device_ = NULL;
	debug("device closed\n");
}

/* Two bits control LED: 0x21 bit 7 and 0x23 bit 7.
 * (direction and output)? */
void PS3EYECam::ov534_set_led(int status)
{
	uint8_t data;

	debug("led status: %d\n", status);

	data = ov534_reg_read(0x21);
	data |= 0x80;
	ov534_reg_write(0x21, data);

	data = ov534_reg_read(0x23);
	if (status)
		data |= 0x80;
	else
		data &= ~0x80;

	ov534_reg_write(0x23, data);
	
	if (!status) {
		data = ov534_reg_read(0x21);
		data &= ~0x80;
		ov534_reg_write(0x21, data);
	}
}

/* validate frame rate and (if not dry run) set it */
uint8_t PS3EYECam::ov534_set_frame_rate(uint8_t frame_rate, bool dry_run)
{
     int i;
     struct rate_s {
             uint8_t fps;
             uint8_t r11;
             uint8_t r0d;
             uint8_t re5;
     };
     const struct rate_s *r;
     static const struct rate_s rate_0[] = { /* 640x480 */
             {60, 0x01, 0xc1, 0x04},
             {50, 0x01, 0x41, 0x02},
             {40, 0x02, 0xc1, 0x04},
             {30, 0x04, 0x81, 0x02},
             {15, 0x03, 0x41, 0x04},
     };
     static const struct rate_s rate_1[] = { /* 320x240 */
             {205, 0x01, 0xc1, 0x02}, /* 205 FPS: video is partly corrupt */
             {187, 0x01, 0x81, 0x02}, /* 187 FPS or below: video is valid */
             {150, 0x01, 0xc1, 0x04},
             {137, 0x02, 0xc1, 0x02},
             {125, 0x02, 0x81, 0x02},
             {100, 0x02, 0xc1, 0x04},
             {75, 0x03, 0xc1, 0x04},
             {60, 0x04, 0xc1, 0x04},
             {50, 0x02, 0x41, 0x04},
             {37, 0x03, 0x41, 0x04},
             {30, 0x04, 0x41, 0x04},
     };

     if (frame_width == 640) {
             r = rate_0;
             i = ARRAY_SIZE(rate_0);
     } else {
             r = rate_1;
             i = ARRAY_SIZE(rate_1);
     }
     while (--i > 0) {
             if (frame_rate >= r->fps)
                     break;
             r++;
     }
 
     if (!dry_run) {
     sccb_reg_write(0x11, r->r11);
     sccb_reg_write(0x0d, r->r0d);
     ov534_reg_write(0xe5, r->re5);
    }

     debug("frame_rate: %d\n", r->fps);
     return r->fps;
}

void PS3EYECam::ov534_reg_write(uint16_t reg, uint8_t val)
{
	int ret;

	//debug("reg=0x%04x, val=0%02x", reg, val);
	usb_buf[0] = val;

  	ret = libusb_control_transfer(handle_,
							LIBUSB_ENDPOINT_OUT | 
							LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE, 
							0x01, 0x00, reg,
							usb_buf, 1, 500);
	if (ret < 0) {
		debug("write failed\n");
	}
}

uint8_t PS3EYECam::ov534_reg_read(uint16_t reg)
{
	int ret;

	ret = libusb_control_transfer(handle_,
							LIBUSB_ENDPOINT_IN|LIBUSB_REQUEST_TYPE_VENDOR|LIBUSB_RECIPIENT_DEVICE, 
							0x01, 0x00, reg,
							usb_buf, 1, 500);

	//debug("reg=0x%04x, data=0x%02x", reg, usb_buf[0]);
	if (ret < 0) {
		debug("read failed\n");
	
	}
	return usb_buf[0];
}

int PS3EYECam::sccb_check_status()
{
	uint8_t data;
	int i;

	for (i = 0; i < 5; i++) {
		data = ov534_reg_read(OV534_REG_STATUS);

		switch (data) {
		case 0x00:
			return 1;
		case 0x04:
			return 0;
		case 0x03:
			break;
		default:
			debug("sccb status 0x%02x, attempt %d/5\n",
			       data, i + 1);
		}
	}
	return 0;
}

void PS3EYECam::sccb_reg_write(uint8_t reg, uint8_t val)
{
	//debug("reg: 0x%02x, val: 0x%02x", reg, val);
	ov534_reg_write(OV534_REG_SUBADDR, reg);
	ov534_reg_write(OV534_REG_WRITE, val);
	ov534_reg_write(OV534_REG_OPERATION, OV534_OP_WRITE_3);

	if (!sccb_check_status()) {
		debug("sccb_reg_write failed\n");
}
}


uint8_t PS3EYECam::sccb_reg_read(uint16_t reg)
{
	ov534_reg_write(OV534_REG_SUBADDR, (uint8_t)reg);
	ov534_reg_write(OV534_REG_OPERATION, OV534_OP_WRITE_2);
	if (!sccb_check_status()) {
		debug("sccb_reg_read failed 1\n");
	}

	ov534_reg_write(OV534_REG_OPERATION, OV534_OP_READ_2);
	if (!sccb_check_status()) {
		debug( "sccb_reg_read failed 2\n");
	}

	return ov534_reg_read(OV534_REG_READ);
}
/* output a bridge sequence (reg - val) */
void PS3EYECam::reg_w_array(const uint8_t (*data)[2], int len)
{
	while (--len >= 0) {
		ov534_reg_write((*data)[0], (*data)[1]);
		data++;
	}
}

/* output a sensor sequence (reg - val) */
void PS3EYECam::sccb_w_array(const uint8_t (*data)[2], int len)
{
	while (--len >= 0) {
		if ((*data)[0] != 0xff) {
			sccb_reg_write((*data)[0], (*data)[1]);
		} else {
			sccb_reg_read((*data)[1]);
			sccb_reg_write(0xff, 0x00);
		}
		data++;
	}
}

} // namespace