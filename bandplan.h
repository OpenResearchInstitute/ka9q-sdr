#define NOVICE_CLASS 1
#define TECHNICIAN_CLASS 2
#define GENERAL_CLASS 4
#define ADVANCED_CLASS 8
#define EXTRA_CLASS 16

#define VOICE 1
#define DATA 2
#define IMAGE 4
#define CW 8

struct bandplan {
  double lower;
  double upper;
  int classes;
  int modes;
  char name[160];
};

struct bandplan *lookup_frequency(double);
