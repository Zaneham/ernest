//ERNESTQB JOB CLASS=Q,TARGET=IBM,SHOTS=1000
//*
//*  Sample Ernest JCL deck. Three small demos batched into a
//*  single SamplerV2 submission. Run with:
//*
//*    python tools/run_jcl.py examples/demos.jcl
//*
//*  Add --submit to actually send it to IBM.
//*
//STEP01   EXEC CIRCUIT=BELL
//STEP02   EXEC CIRCUIT=GHZ,SHOTS=2000
//STEP03   EXEC CIRCUIT=GROVER
//END
