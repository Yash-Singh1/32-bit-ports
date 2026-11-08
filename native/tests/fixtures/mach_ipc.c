#include <mach/mach.h>
#include <stdint.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

extern unsigned char WaitNextEvent(uint16_t, void *, uint32_t, void *);
int check_mach_ipc(void)
{
    _Static_assert(sizeof(mach_msg_header_t) == 24, "i386 Mach header");
    struct { uint32_t before; unsigned char event[16]; uint32_t after; } event = {0x11223344, {0}, 0xaabbccdd};
    memset(event.event, 0xff, sizeof(event.event));
    if (WaitNextEvent(0, event.event, 0, 0) || event.event[0] || event.event[1] ||
        event.before != 0x11223344 || event.after != 0xaabbccdd) return -613;
    mach_port_t self = mach_thread_self();
    mach_port_type_t self_type = 0;
    if (!self || mach_port_type(mach_task_self(), self, &self_type) ||
        !(self_type & MACH_PORT_TYPE_SEND) || mach_port_deallocate(mach_task_self(), self)) return -611;
    struct { uint32_t before, words[10], after; } info = {0x11223344, {0}, 0xaabbccdd};
    mach_msg_type_number_t count = 10;
    if (task_info(mach_task_self(), 5, (task_info_t)info.words, &count) || count != 10 ||
        info.before != 0x11223344 || info.after != 0xaabbccdd) return -612;
    mach_port_t channel = 0;
    if (kill(getpid(), 0) || mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &channel)) return -601;
    struct { mach_port_type_t type; uint32_t guard; } type = {0, 0x1234abcd};
    if (mach_port_type(mach_task_self(), channel, &type.type) ||
        !(type.type & MACH_PORT_TYPE_RECEIVE) || type.guard != 0x1234abcd) return -602;
    struct { mach_msg_header_t head; uint32_t value; mach_msg_trailer_t trailer; } msg = {0};
    msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    msg.head.msgh_size = sizeof(msg.head) + 4;
    msg.head.msgh_remote_port = channel;
    msg.head.msgh_id = 42;
    msg.value = 0x76543210;
    if (mach_msg(&msg.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg.head.msgh_size, 0, 0, 100, 0)) return -603;
    memset(&msg, 0, sizeof(msg));
    if (mach_msg(&msg.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), channel, 100, 0) ||
        msg.head.msgh_id != 42 || msg.value != 0x76543210) return -604;
    if (mach_msg(&msg.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), channel, 1, 0) != MACH_RCV_TIMED_OUT) return -605;
    struct {
        mach_msg_header_t head; mach_msg_body_t body;
        mach_msg_port_descriptor_t port; mach_msg_trailer_t trailer;
    } complex = {0};
    complex.head.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    complex.head.msgh_size = sizeof(complex) - sizeof(complex.trailer);
    complex.head.msgh_remote_port = channel;
    complex.body.msgh_descriptor_count = 1;
    complex.port.name = channel;
    complex.port.disposition = MACH_MSG_TYPE_MAKE_SEND;
    complex.port.type = MACH_MSG_PORT_DESCRIPTOR;
    if (mach_msg(&complex.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, complex.head.msgh_size, 0, 0, 100, 0)) return -606;
    memset(&complex, 0, sizeof(complex));
    if (mach_msg(&complex.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(complex), channel, 100, 0) ||
        complex.body.msgh_descriptor_count != 1 || complex.port.name != channel) return -607;
    if (mach_port_deallocate(mach_task_self(), complex.port.name)) return -608;
    // Never pass an i386 out-of-line pointer descriptor directly to the kernel.
    complex.head.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    complex.head.msgh_remote_port = channel;
    complex.port.type = MACH_MSG_OOL_DESCRIPTOR;
    if (mach_msg(&complex.head, MACH_SEND_MSG, complex.head.msgh_size, 0, 0, 0, 0) != MACH_SEND_INVALID_TYPE) return -609;
    if (mach_port_mod_refs(mach_task_self(), channel, MACH_PORT_RIGHT_RECEIVE, -1)) return -610;
    return 0;
}
