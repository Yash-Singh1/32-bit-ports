#ifndef LP32_QTKIT_BRIDGE_H
#define LP32_QTKIT_BRIDGE_H

#include <stdint.h>

/*
 * QTKit (removed from macOS) stand-ins on AVFoundation: LP32QTMovie and
 * LP32QTMovieView, which objc_legacy32_class hands out for the guest names
 * QTMovie and QTMovieView, so guest subclasses (Gangstar's MovieView) get a
 * working superclass.
 */
extern const char *const kQTMovieDidEndNotificationName;
void *qtkit_bridge32_class(const char *guest_name);

/* QTTime results and arguments in the guest's i386 layout
   {int64 timeValue; int32 timeScale; int32 flags}.  Return 1 if handled. */
int qtkit_bridge32_stret(void *receiver, const char *selector, void *destination);
int qtkit_bridge32_message(void *receiver, const char *selector, const uint32_t *arguments, uint64_t *result);

#endif
