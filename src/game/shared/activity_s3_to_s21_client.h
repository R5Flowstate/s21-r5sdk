//=============================================================================//
//
// Purpose: translate wire-delivered S3 activity IDs to their S21 equivalents
// on the client.
//
//=============================================================================//
#ifndef ACTIVITY_S3_TO_S21_CLIENT_H
#define ACTIVITY_S3_TO_S21_CLIENT_H

// Translate an S3 activity ID to its S21 equivalent. Returns the input
// unchanged if no translation entry exists (identity / pass-through). Hot path
// binary search over a sorted 737-entry table -> O(log n), ~10 compares max.
int Bridge_TranslateS3ActivityToS21_Static(int s3_id);

#endif // ACTIVITY_S3_TO_S21_CLIENT_H
