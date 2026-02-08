#pragma once
#include "experiments/experiment.h"

int Experiments_Count(void);
const Experiment* Experiments_GetByIndex(int index);
const Experiment* Experiments_GetById(int id);
