#ifndef PROCEDURE_H
#define PROCEDURE_H

void procedureBegin();
void procedureStart();
void procedureStop();
void procedureSetManualOutputs(bool filling, bool emptying);
void procedureUpdate();
void procedureEncoderTurnBeep();
void procedureEncoderPressBeep();
void procedureEncoderErrorBeep();
void procedureRequestSoftwareReset();

#endif