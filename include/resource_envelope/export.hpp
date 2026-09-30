#ifndef RESOURCE_ENVELOPE_EXPORT_HPP
#define RESOURCE_ENVELOPE_EXPORT_HPP

// Resource Envelope is built and consumed as a static library, so the visibility
// macros resolve to nothing. They exist so that shared-library builds remain
// possible without editing public headers.

#if defined(_WIN32) && defined(RESOURCE_ENVELOPE_SHARED)
#  if defined(RESOURCE_ENVELOPE_BUILDING_LIBRARY)
#    define RESOURCE_ENVELOPE_API __declspec(dllexport)
#  else
#    define RESOURCE_ENVELOPE_API __declspec(dllimport)
#  endif
#else
#  define RESOURCE_ENVELOPE_API
#endif

#endif  // RESOURCE_ENVELOPE_EXPORT_HPP
