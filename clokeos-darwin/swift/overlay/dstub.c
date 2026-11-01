/* Daten-Symbole aus libdispatch, die PureDarwins libSystem nicht liefert (nur gebunden, nicht benutzt) */
long _dispatch_data_empty[16];
void *_dispatch_data_destructor_free, *_dispatch_data_destructor_munmap;
void _swift_dispatch_source_create_abort(void) { __builtin_trap(); }
void *NSURLErrorBackgroundTaskCancelledReasonKey, *NSURLErrorNetworkUnavailableReasonKey, *NSURLSessionDownloadTaskResumeData;
