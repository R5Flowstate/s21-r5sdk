//=============================================================================//
//
// Purpose: S3->S21 activity ID translation for the bridge wire.
//
//=============================================================================//
#ifndef ACTIVITY_S3_TO_S21_H
#define ACTIVITY_S3_TO_S21_H

// First call after LoadCustomActivitiesFromFile builds the map; later calls no-op.
void Bridge_BuildS3ToS21ActivityMap();

// S3 activity id -> S21 id. Unmapped names emit 0 (tables are dense). id<=0
// and pre-init pass through. bridge_act_xlat_unmapped 0 restores raw pass-through.
int  Bridge_TranslateS3ActivityToS21(int s3_id);

// [WEAP-ACT-C2S] S21->S3 at the usercmd boundary; S3->S21 proxy maps it back.
int  Bridge_TranslateS21ActivityToS3(int s21_id);

#endif // ACTIVITY_S3_TO_S21_H
