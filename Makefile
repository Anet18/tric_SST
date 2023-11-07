CXX = mpicxx
#CXX = sst++
DBGFLAGS = -fPIC -g -fomit-frame-pointer
OPTFLAGS = $(DBGFLAGS) -O3 -DPRINT_EXTRA_NEDGES  
# -DPRINT_EXTRA_NEDGES prints extra edges when -p <> is passed to 
#  add extra edges randomly on a generated graph
# use export ASAN_OPTIONS=verbosity=1 to check ASAN output
SNTFLAGS = -std=c++11 -fsanitize=address -O1 -fno-omit-frame-pointer
CXXFLAGS = -std=c++11 $(OPTFLAGS) -I.
LDFLAGS = 

OBJ = main.o
TARGET = tric

all: $(TARGET)

$(TARGET):  $(OBJ)
	$(LDAPP) $(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) 

.PHONY: clean

clean:
	rm -rf *~ $(OBJ) $(TARGET) *.dSYM
