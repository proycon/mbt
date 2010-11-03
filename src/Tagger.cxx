/*
  Copyright (c) 1998 - 2010
  ILK  -  Tilburg University
  CNTS -  University of Antwerp
 
  This file is part of Mbt3

  Mbt3 is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  Mbt3 is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <http://www.gnu.org/licenses/>.

  For questions and suggestions, see:
      http://ilk.uvt.nl/software.html
  or send mail to:
      Timbl@uvt.nl
*/

#include <algorithm> 
#include <fstream> 
#include <iostream>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <csignal>
#include <cassert>

#include <unistd.h> // for unlink()
#include "config.h"
#include "timbl/TimblAPI.h"
#include "timblserver/TimblServerAPI.h"
#include "mbt/TagLex.h"
#include "mbt/Pattern.h"
#include "mbt/Sentence.h"
#include "mbt/Logging.h"
#include "mbt/Tagger.h"

#if defined(PTHREADS)
#include <pthread.h>
#endif

LogStream default_log( std::cerr ); // fall-back
LogStream *cur_log = &default_log;  // fill the externals

LogLevel internal_default_level = LogNormal;
LogLevel Tagger_Log_Level       = internal_default_level;

namespace Tagger {
  using namespace std;
  using namespace Hash;
  using namespace Timbl;
  using namespace Sockets;

  const string UNKSTR   = "UNKNOWN";
  
  Lexicon MT_lexicon;
  StringHash kwordlist;
  StringHash uwordlist;
  
  class BeamData;
  
  TaggerClass::TaggerClass( ){
    cur_log->setlevel( LogNormal );
    cur_log->setstamp( StampMessage );
    KnownTree = NULL;
    unKnownTree = NULL;
    mySentence = new sentence();
    TheLex = new StringHash();
    TimblOptStr = "+vS -FColumns K: -a IGTREE +D U: -a IB1 ";
    FilterTreshold = 5;
    Npax = 5;
    TopNumber = 100;
    DoSort = false;
    DoTop = false;
    DoNpax = true;
    KeepIntermediateFiles = false;

    KtmplStr = "ddfa";
    UtmplStr = "dFapsss";
    L_option_name = "";
    EosMark = "<utt>";
    
    portnumstr = "";
    Max_Conn_Str = "10";

    UnknownTreeName = "";
    KnownTreeName = "";  
    LexFileName = "";
    MTLexFileName = "";
    TopNFileName = "";
    NpaxFileName = "";
    TestFileName = "";
    OutputFileName = "";
    SettingsFileName = "";
    SettingsFilePath = "";
  
    TestPat = new vector<int>;
    initialized = false;
    Beam_Size = 1;
    Beam = NULL;
    piped_input = true;
    input_kind = UNTAGGED;
    lexflag = false;
    knowntreeflag = false;
    unknowntreeflag = false;
    knowntemplateflag = false;
    unknowntemplateflag = false;
    knownoutfileflag = false;
    unknownoutfileflag = false;
    reverseflag = false;
    dumpflag = false;
    distance_flag = false;
    distrib_flag = false;
    klistflag= false;
    servermode = false;
    Sock = 0;
  }

  bool setLog( LogStream& os ){
    cur_log = new LogStream( os, "mbt-" );
    return true;
  }

  BeamData::BeamData(){
    size = 0;
    paths = 0;
    temppaths = 0;
    path_prob = 0;
    n_best_array = 0;
  }

  BeamData::~BeamData(){
    if ( paths ){
      for ( int q=0; q < size; q++ ){
	delete n_best_array[q];
	delete [] paths[q];
	delete [] temppaths[q];
      }
    }
    delete [] paths;
    delete [] temppaths;
    delete [] path_prob;
    delete [] n_best_array;
  }
  
  const string& indexlex( const unsigned int index,
			  StringHash& aLex){
    return aLex.ReverseLookup( index );
  }


  bool BeamData::Init( int Size, unsigned int noWords ){
    // Beaming Stuff...
    if ( path_prob == 0 ){
      // the first time
      if ( (path_prob = new double[Size]) == 0 ||
	   (n_best_array = new n_best_tuple*[Size]) == 0 ||
	   (paths = new int*[Size]) == 0 ||
	   (temppaths = new int*[Size]) == 0 ){
	cerr << "not enough memory for N-best search tables " << endl;
	return false;
      }
      else {
	for ( int q=0; q < Size; q++ ){
	  paths[q] = 0;
	  temppaths[q] = 0;
	  if ( (n_best_array[q] = new n_best_tuple) == 0 ){
	    cerr << "not enough memory for N-best search tables " << endl;
	    return false;
	  }
	}
      }
    }
    else {
      for ( int q=0; q < Size; ++q ){
	delete [] paths[q];
	delete [] temppaths[q];
      }
    }
    for ( int q=0; q < Size; q++ ){
      if ( (paths[q] = new int[noWords]) == 0 ||
	   (temppaths[q] = new int[noWords]) == 0 ){
	cerr << "not enough memory for N-best search tables " << endl;
	return false;
      }
    }
    size = Size;
    return true;
  }
  
  void BeamData::ClearBest(){
    DBG << "clearing n_best_array..." << endl;
    for ( int i=0; i < size; i++ )
      n_best_array[i]->clean();
  }

  void BeamData::Shift(	int no_words, int i_word ){
    for ( int q1 = 0; q1 < no_words; q1++ ){
      for ( int jb = 0; jb < size; jb++ ){
	path_prob[jb] = n_best_array[jb]->prob;
	if ( n_best_array[jb]->path != EMPTY_PATH ){
	  if ( q1 < i_word ){
	    DBG << "shift paths[" << n_best_array[jb]->path << ","
		<< q1 << "] into paths[" << jb << "," << q1 << "]" << endl;
	    temppaths[jb][q1] = paths[n_best_array[jb]->path][q1];
	  }
	  else if ( q1 == i_word ){
	    DBG << "shift tag " <<  n_best_array[jb]->tag 
		<< " into paths[" << jb << "," << q1 << "]" << endl;
	    temppaths[jb][q1] = n_best_array[jb]->tag;
	  }
	  else
	    temppaths[jb][q1] = EMPTY_PATH;
	}
	else
	  temppaths[jb][q1] = EMPTY_PATH;
      }
    }
    for ( int jb = 0; jb < size; jb++ ){
      for ( int q1=0; q1 < no_words; q1++ )
	paths[jb][q1] = temppaths[jb][q1];
    }
  }
  
  void BeamData::Print( ostream& os, int i_word, StringHash& TheLex ){  
    for ( int i=0; i < size; ++i ){
      os << "path_prob[" << i << "] = " << path_prob[i] << endl;
    }
    for ( int j=0; j <= i_word; ++j ){
      for ( int i=0; i < size; ++i ){
	if (  paths[i][j] != EMPTY_PATH ){
	  LOG << "    paths[" << i << "," << j << "] = " 
	      << indexlex( paths[i][j], TheLex ) << endl;
	}
	else {
	  LOG << "    paths[" << i << "," << j << "] = EMPTY" << endl;
	}
      }
    }
  }
  
  void BeamData::PrintBest( ostream& os, StringHash& TheLex ){
    for ( int i=0; i < size; ++i ){
      if (  n_best_array[i]->path != EMPTY_PATH ){
	os << "n_best_array[" << i << "] = " 
	   << n_best_array[i]->prob << " "
	   << n_best_array[i]->path << " "
	   << indexlex( n_best_array[i]->tag, TheLex ) << endl;
      }
      else {
	os << "n_best_array[" << i << "] = " 
	    << n_best_array[i]->prob << " EMPTY " << endl;
      }
    }
  }
  
  class name_prob_pair{
  public:
    name_prob_pair( const string& n, double p ){
      name = n; prob = p; next = 0;
    }
    ~name_prob_pair(){};
    string name;
    double prob;
    name_prob_pair *next;
  };

  name_prob_pair *add_descending( name_prob_pair *n, name_prob_pair *lst ){
    name_prob_pair *result;
    if ( lst == 0 )
      result = n;
    else if ( n->prob - lst->prob >= 0 ){
      result = n;
      n->next = lst;
    }
    else {
      result = lst;
      result->next = add_descending( n, result->next );
    }
    return result;
  }
  

  name_prob_pair *break_down( const ValueDistribution *Dist,
			      const TargetValue *PrefClass ){
    // split a distribution into names/probabilities  AND sort them descending
    // But put preferred in front. 
    // While we will use only the first BeamSize entries, don't forget
    // the most important one...!
    name_prob_pair *result = 0, *tmp, *Pref = 0;
    double sum_freq = 0.0;
    ValueDistribution::dist_iterator it = Dist->begin();
    while ( it != Dist->end() ){
      string name = (*it).second->Value()->Name();
      double freq = (*it).second->Weight();
      sum_freq += freq;
      tmp = new name_prob_pair( name, freq );
      if ( name == PrefClass->Name() )
	Pref = tmp;
      else
	result = add_descending( tmp, result );
      ++it;
    }
    if ( Pref ){
      Pref->next = result;
      result = Pref;
    }
    //
    // Now we must Normalize te get real Probalilities
    tmp = result;
    while ( tmp ){
      tmp->prob = tmp->prob / sum_freq;
      tmp = tmp->next;
    }
    return result;
  }

  void BeamData::InitPaths( StringHash& TheLex,
			    const TargetValue *answer,
			    const ValueDistribution *distrib ){
    if ( size == 1 ){
      paths[0][0] = TheLex.Hash( answer->Name() );
      path_prob[0] = 1.0;
    }
    else {
      name_prob_pair *d_pnt, *tmp_d_pnt, *Distr;
      Distr = break_down( distrib, answer );
      d_pnt = Distr;
      int jb = 0;
      while ( d_pnt ){
	if ( jb < size ){
	  paths[jb][0] =  TheLex.Hash( d_pnt->name );
	  path_prob[jb] = d_pnt->prob;
	}
	tmp_d_pnt = d_pnt;
	d_pnt = d_pnt->next;
	delete tmp_d_pnt;
	jb++;
      }
      for ( ; jb < size; jb++ ){
	paths[jb][0] = EMPTY_PATH;
	path_prob[jb] = 0.0;
      }
    }
  }

  void BeamData::NextPath( StringHash& TheLex,
			   const TargetValue *answer,
			   const ValueDistribution *distrib,
			   int beam_cnt ){
    if ( size == 1 ){
      n_best_array[0]->prob = 1.0;
      n_best_array[0]->path = beam_cnt;
      n_best_array[0]->tag = TheLex.Hash( answer->Name() );
    }
    else {
      DBG << "BeamData::NextPath[" << beam_cnt << "] ( " << answer << " , " 
	  << distrib << " )" << endl;
      name_prob_pair *d_pnt, *tmp_d_pnt, *Distr;
      Distr = break_down( distrib, answer );
      d_pnt = Distr;
      int ab = 0;
      while ( d_pnt ){
	if ( ab < size ){
	  double thisWProb = d_pnt->prob;
	  double thisPProb = thisWProb * path_prob[beam_cnt];
	  int dtag = TheLex.Hash( d_pnt->name );
	  for( int ane = size-1; ane >=0; ane-- ){
	    if ( thisPProb <= n_best_array[ane]->prob )
	      break;
	    if ( ane == 0 ||
		 thisPProb <= n_best_array[ane-1]->prob ){
 	      if ( ane == 0 )
 		DBG << "Insert, n=0" << endl;
 	      else
 		DBG << "Insert, n=" << ane << " Prob = " << thisPProb
		    << " after prob = " << n_best_array[ane-1]->prob 
		    << endl;
	      // shift
	      n_best_tuple *keep = n_best_array[size-1];
	      for ( int ash = size-1; ash > ane; ash-- ){
		n_best_array[ash] = n_best_array[ash-1];
	      }
	      n_best_array[ane] = keep;
	      n_best_array[ane]->prob = thisPProb;
	      n_best_array[ane]->path = beam_cnt;
	      n_best_array[ane]->tag = dtag;
	    }
	  }
	}
	tmp_d_pnt = d_pnt;
	d_pnt = d_pnt->next;
	delete tmp_d_pnt;
	++ab;
      }
    }
  }
  
  TaggerClass::~TaggerClass(){
    if ( Sock == 0 ){
      delete KnownTree;
      delete unKnownTree;
    }
    else
      delete Sock;
    delete Beam;
    delete TestPat;
    delete mySentence;    
    delete TheLex;
  }
  
  bool TaggerClass::InitBeaming( unsigned int no_words ){
    if ( !Beam ){
      Beam = new BeamData();
    }
    return Beam->Init( Beam_Size, no_words );
  }

#if defined(PTHREADS)

  static pthread_mutex_t my_lock = PTHREAD_MUTEX_INITIALIZER;
  static int service_count = 0;

  void StopServerFun( int Signal ){
    if ( Signal == SIGINT ){
      exit(EXIT_FAILURE);
    }
    signal( SIGINT, StopServerFun );
  }
  
  // ***** This is the routine that is executed from a new thread **********
  void *tag_child( void *arg ){
    TaggerClass *tagger = (TaggerClass *)arg;
    tagger->DoChild();
    delete tagger;
    return NULL;
  }

  void TaggerClass::DoChild(){
    // process the test material
    // and do the timing
    //
    // use a mutex to update the global service counter
    //
    pthread_mutex_lock( &my_lock );
    service_count++;
    int nw = 0;
    if ( service_count > Max_Connections ){
      Sock->write( "Maximum connections exceeded\n" );
      Sock->write( "try again later...\n" );
      pthread_mutex_unlock( &my_lock );
      cerr << "Thread " << pthread_self() << " refused " << endl;
    }
    else {
      // Greeting message for the client
      //
      pthread_mutex_unlock( &my_lock );
      time_t timebefore, timeafter;
      time( &timebefore );
      // report connection to the server terminal
      //
      LOG << "Thread " << pthread_self() << ", Socket number = "
	  << Sock->getSockId() << ", started at: " 
	  << asctime( localtime( &timebefore) );
      Sock->write( "Welcome to the Mbt server.\n" );
      nw = ProcessSocket();
      time( &timeafter );
      LOG << "Thread " << pthread_self() << ", terminated at: " 
	  << asctime( localtime( &timeafter ) );
      LOG << "Total time used in this thread: " << timeafter - timebefore 
	  << " sec, " << nw << " words processed " << endl;
    }
    // exit this thread
    //
    pthread_mutex_lock( &my_lock );
    service_count--;
    LOG << "Socket Total = " << service_count << endl;
    pthread_mutex_unlock( &my_lock );
  }
  
  bool TaggerClass::RunServer(){
    if ( initialized ){
      Max_Connections = stringTo<int>( Max_Conn_Str );
      if ( Max_Connections < 1 ){
	cerr << "Error in -C option, setting Max_Connections to 10" << endl;
	Max_Connections = 10;
      }
      cerr << "Trying to Start a Server on port: " << portnumstr << endl
	   << "maximum # of simultanious connections: " << Max_Connections
	   << endl;

      if ( !logFile.empty() ){
	ostream *tmp = new ofstream( logFile.c_str() );
	if ( tmp && tmp->good() ){
	  cerr << "switching logging to file " << logFile << endl;
	  cur_log->associate( *tmp );
	  cur_log->message( "MbtServer:" );
	  LOG << "Started logging " << endl;	
	}
	else {
	  cerr << "unable to create logfile: " << logFile << endl;
	  cerr << "not started" << endl;
	  exit(EXIT_FAILURE);
	}
      }
      int start;
      if ( logFile.empty() )
	start = daemonize( 1, 1 );
      else
	start = daemonize( 0, 0 );
      if ( start < 0 ){
	LOG << "failed to daemonize error= " << strerror(errno) << endl;
	exit(EXIT_FAILURE);
      };
      if ( !pidFile.empty() ){
	// we have a liftoff!
	// signal it to the world
	unlink( pidFile.c_str() ) ;
	ofstream pid_file( pidFile.c_str() ) ;
	if ( !pid_file ){
	  LOG << "unable to create pidfile:"<< pidFile << endl;
	  LOG << "TimblServer NOT Started" << endl;
	  exit(EXIT_FAILURE);
	}
	else {
	  pid_t pid = getpid();
	  pid_file << pid << endl;
	}
      }

      // set the attributes
      pthread_attr_t attr;
      if ( pthread_attr_init(&attr) ||
	   pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED ) ){
	LOG << "Threads: couldn't set attributes" << endl;
	exit(EXIT_FAILURE);
      }
      //
      // setup Signal handling to abort the server.
      signal( SIGINT, StopServerFun );
      
      pthread_t chld_thr;

      // start up server
      // 
      LOG << "Started Server on port: " << portnumstr << endl
	  << "maximum # of simultanious connections: " << Max_Connections
	  << endl;
      
      ServerSocket server;
      if ( !server.connect( portnumstr ) ){
	LOG << "failed to start Server: " << server.getMessage() << endl;
	exit(EXIT_FAILURE);
      }
      if ( !server.listen( Max_Connections ) < 0 ){
	LOG << "server: listen failed " << strerror( errno ) << endl;
	exit(EXIT_FAILURE);
      };
      
      while(true){ // waiting for connections loop
	ServerSocket *newSock = new ServerSocket();
	if ( !server.accept( *newSock ) ){
	  if( errno == EINTR )
	    continue;
	  else {
	    LOG << "Server: Accept Error: " << server.getMessage() << endl;
	    exit(EXIT_FAILURE);
	  }
	}
	LOG << "Accepting Connection " << newSock->getSockId()
	    << " from remote host: " << newSock->getClientName() << endl;
	
	// create a new thread to process the incoming request 
	// (The thread will terminate itself when done processing
	// and release its socket handle)
	//
	TaggerClass *child = clone( newSock );
	pthread_create( &chld_thr, &attr, tag_child, child );
	// the server is now free to accept another socket request 
      }
      return true;
    }
    else {
      return false;
    }
  }

  int TaggerClass::ProcessSocket(){
    bool go_on = true;
    int no_words=0;    
    // loop as long as you get sentences
    //
    string tagged_sentence;
    while ( go_on &&
	    ( mySentence->reset( EosMark ), mySentence->read( Sock, input_kind ) )) {
      if ( Tag( tagged_sentence ) ){
	// show the results of 1 sentence
	go_on = Sock->write( tagged_sentence );
	// increase the counter of processed words
	no_words += mySentence->No_Words();
      }
      else {
	// probably empty sentence??
      }
    } // end of while looping over sentences
    
    cerr << endl << endl << "Done:" << endl
	 << "  " << no_words << " words processed." << endl;
    return no_words;
  }
  

#else
  bool TaggerClass::RunServer(){
    cerr << "Sorry: Servermode not available. " << endl;
    return false;
  }

  int TaggerClass::ProcessSocket( ){
    return -1;
  }
#endif  

  void get_weightsfile_name( string& opts, string& name ){
    name = "";
    string::size_type pos = opts.find( "-w" );
    if ( pos != string::npos ){
      string::size_type b_pos = opts.find_first_not_of( " \t\r", pos+2 );
      string::size_type e_pos = opts.find_first_of( " \t\r", b_pos );
      string tmp = opts.substr( b_pos, e_pos - b_pos );
      Weighting W;
      if ( !string_to( tmp, W ) ){
	// no weight, so assume a filename...
	name = tmp;
	opts.erase( pos, e_pos - pos );
      }
    }
  }
  

  void splits( const string& opts, string& known, string& unknown,
	       string& kwf, string& uwf ){
    xDBG << "splits, opts = " << opts << endl;
    known = "";
    unknown = "";
    string common = " -FColumns ";
    bool done_u = false, done_k = false;
    string::size_type k_pos = opts.find( "K:" );
    string::size_type u_pos = opts.find( "U:" );
    xDBG << "K pos " << k_pos << endl;
    xDBG << "U pos " << u_pos << endl;
    if ( k_pos != string::npos ){
      if ( k_pos < u_pos ){
	common += opts.substr( 0, k_pos );
	known = opts.substr( k_pos+2, u_pos - k_pos - 2 );
      }
      else
	known = opts.substr( k_pos+2 );
      done_k = true;
    }
    if ( u_pos != string::npos ){
      if ( u_pos < k_pos ){
	common += opts.substr( 0, u_pos );
	unknown = opts.substr( u_pos+2, k_pos - u_pos - 2 );
      }
      else
	unknown = opts.substr( u_pos+2 );
      done_u = true;
    }
    if ( !done_u ){
      if ( !done_k ) {
	known = opts;
	unknown = opts;
      }
      else if ( k_pos != string::npos ){
	unknown = opts.substr( 0, k_pos );
      }
      else
	unknown = known;
    }
    else if ( !done_k ) {
      if ( u_pos != string::npos ){
	known = opts.substr( 0, u_pos );
      }
      else
	known = unknown;
    }
    xDBG << "resultaat splits, common = " << common << endl;
    xDBG << "resultaat splits, K = " << known << endl;
    xDBG << "resultaat splits, U = " << unknown << endl;
    known += common;
    unknown += common;
    get_weightsfile_name( known, kwf );
    get_weightsfile_name( unknown, uwf );
  }
  
  bool TaggerClass::set_default_filenames( ){
    //
    // and use them to setup the defaults...
    if( KtmplStr[0] ) {
      if ( Ktemplate.set( KtmplStr ) )
	knowntemplateflag = true;
      else {
	cerr << "couldn't set Known Template from '" << KtmplStr
	     << "'" << endl;
	return false;
      }
    }
    if( UtmplStr[0] ) {
      if ( Utemplate.set( UtmplStr ) )
	unknowntemplateflag = true;
      else {
	cerr << "couldn't set Unknown Template from '" << UtmplStr 
	     << "'" << endl;
	return false;
      }
    }
    char affix[32];
    LexFileName = TestFileName;
    LexFileName += ".lex";
    if ( FilterTreshold < 10 )
      sprintf( affix, ".0%1i",  FilterTreshold );
    else
      sprintf( affix, ".%2i",  FilterTreshold );
    if( !knownoutfileflag ){
      K_option_name = TestFileName;
      K_option_name += ".known.inst.";
      K_option_name += KtmplStr;
    }
    if ( !knowntreeflag ){
      KnownTreeName = TestFileName;
      KnownTreeName += ".known.";
      KnownTreeName += KtmplStr;
    }
    if( !unknownoutfileflag ){
      U_option_name = TestFileName;
      U_option_name += ".unknown.inst.";
      U_option_name += UtmplStr;
    }
    if ( !unknowntreeflag ){
      UnknownTreeName = TestFileName;
      UnknownTreeName += ".unknown.";
      UnknownTreeName += UtmplStr;
    }
    if( lexflag ){
      MTLexFileName = l_option_name;
    }
    else {
      MTLexFileName = TestFileName;
      MTLexFileName += ".lex.ambi";
      MTLexFileName +=  affix;
    }
    if ( !L_option_name.empty() ) 
      TopNFileName = L_option_name;
    else {
      sprintf( affix, ".top%d",  TopNumber );
      TopNFileName = TestFileName + affix;
    }
    sprintf( affix, ".%dpaxes",  Npax );
    NpaxFileName = TestFileName + affix;
    return true;
  }
    
  void TaggerClass::ProcessTags( TagInfo *TI ){
    TI->Prune( FilterTreshold );
    TI->CreateStringRepr();
  }
  
  bool split_special( const string& Buffer, string& Word, string& Tag ){
    vector<string> subs;
    size_t len = split( Buffer, subs );
    if ( len > 1 ){
      Word = subs.front();
      Tag = subs.back();
      return true;
    }
    return false;
  }
  
  void TaggerClass::create_lexicons( const string& filename ){
    TagLex TaggedLexicon;
    ifstream lex_file;
    ofstream out_file;
    string Buffer;
    if ( filename != "" ){
      if ( ( lex_file.open( filename.c_str(), ios::in ),
	     !lex_file.good() ) ){
	cerr << "couldn't open tagged lexicon file `"
	     << filename << "'" << endl;
	exit(EXIT_FAILURE);
      }
      cout << "Constructing a tagger from: " << filename << endl;
    }
    else {
      cerr << "couldn't open inputfile " << filename << endl;
      exit(EXIT_FAILURE);
    }
    string Word, Tag;
    while ( getline( lex_file, Buffer ) ){ 
      if ( split_special( Buffer, Word, Tag ) ){
	TaggedLexicon.Store( Word, Tag );
      }
    }
    int LexSize = TaggedLexicon.numOfLexiconEntries();
    vector<TagInfo *>TagVect = TaggedLexicon.CreateSortedVector();
    if ( (out_file.open( LexFileName.c_str(), ios::out ), 
	  out_file.good() ) ){
      cout << "  Creating lexicon: "  << LexFileName << " of " 
	   << LexSize << " entries." << endl;
      for ( int i=0; i < LexSize; i++ )
	out_file << TagVect[i]->Freq() << " " << TagVect[i]->Word
		 << " " << TagVect[i]->DisplayTagFreqs() << endl;
      out_file.close();
    }
    else {
      cerr << "couldn't open lexiconfile " << LexFileName << endl;
      exit(EXIT_FAILURE);
    }
    for ( int i=0; i < LexSize; i++ )
      ProcessTags( TagVect[i] );
    if ( (out_file.open( MTLexFileName.c_str(), ios::out ), 
	  out_file.good() ) ){
      cout << "  Creating ambitag lexicon: "  << MTLexFileName << endl;
      for ( int j=0; j < LexSize; j++ ){
	out_file << TagVect[j]->Word << " " << TagVect[j]->stringRep() << endl;
	MT_lexicon.Store(TagVect[j]->Word, TagVect[j]->stringRep() );
      }
      out_file.close();
    }
    else {
      cerr << "couldn't create file: " << MTLexFileName << endl;
      exit(EXIT_FAILURE);
    }
    if ( (out_file.open( TopNFileName.c_str(), ios::out ), 
	  out_file.good() ) ){
      cout << "  Creating list of most frequent words: "  << TopNFileName << endl;
      for ( int k=0; k < LexSize && k < TopNumber; k++ ){
	out_file << TagVect[k]->Word << endl;
	kwordlist.Hash( TagVect[k]->Word );
      }
      out_file.close();
    }
    else {
      cerr << "couldn't open file: " << TopNFileName << endl;
      exit(EXIT_FAILURE);
    } 
    if ( DoNpax ){
      if ( (out_file.open( NpaxFileName.c_str(), ios::out ), 
	    out_file.good() ) ){
	int np_cnt = 0;
	//	cout << "  Creating Npax file: "  << NpaxFileName;
	for ( int l=0; l < LexSize; l++ ){
	  if ( TagVect[l]->Freq() > Npax ) continue;
	  out_file << TagVect[l]->Word << endl;
	  uwordlist.Hash( TagVect[l]->Word );
	  np_cnt++;
	}
	out_file.close();
	//	cout << "( " << np_cnt << " entries)" << endl;
      }
      else {
	cerr << "couldn't open file: " << NpaxFileName << endl;
	exit(EXIT_FAILURE);
      } 
    }
  }
  
  void TaggerClass::ShowCats( ostream& os, vector<int>& Pat, int slots ){
    os << "Pattern : ";
    for( int slot=0; slot < slots; slot++){
      os << indexlex( Pat[slot], *TheLex )<< " ";
    }
    os << endl;
  }
  
  string TaggerClass::pat_to_string( int slots, int word ){
    string line;
    for( int f=0; f < slots; f++ ){
      line += indexlex( (*TestPat)[f], *TheLex );
      line += " ";
    }
    for (vector<string>::iterator it = mySentence->getWord(word)->extraFeatures.begin(); it != mySentence->getWord(word)->extraFeatures.end(); it++)
    {
      line += *it;
      line += " ";
    }
    if ( input_kind == TAGGED )
      line += mySentence->gettag(word);
    else
      line += "??";
    if ( IsActive(DBG) ){
      ShowCats( LOG, *TestPat, slots );
    }
    // dump if desired
    //
    if ( dumpflag ){
      for( int slot=0; slot < slots; slot++){
	cout << indexlex( (*TestPat)[slot], *TheLex );
      }
      cout << endl;
    }
    return line;
  }
  
  void read_lexicon( const string& FileName ){
    string wordbuf;
    string valbuf;
    int no_words=0;
    ifstream lexfile( FileName.c_str(), ios::in);  
    while ( lexfile >> wordbuf >> valbuf ){
      MT_lexicon.Store( wordbuf, valbuf );
      no_words++;
      lexfile >> ws;
    }
    LOG << "  Reading the lexicon from: " << FileName << " ("
	<< no_words << " words)." << endl;
  }

  bool old_style( const string& name ){
    string line;
    ifstream in( name.c_str() );
    if ( in ){
      getline( in, line );
      vector<string> tmp;
      size_t count = split_at_first_of( line, tmp, ",." );
      if ( count <= 0 )
	return false;
      for( size_t i = 0; i < count; ++i ){
	if ( tmp[i].length() < 2 || tmp[i].length() > 3 )
	  return false;
	if ( !isalpha(tmp[i][0]) || !isalpha(tmp[i][1] ) )
	  return false;
	if ( tmp[i].length() == 3 && !isalpha(tmp[i][2] ) )
	  return false;
      }
      return true;
    }
    else
      return false;
  }

  //
  // File should contain one word per line.
  //
  void read_listfile( const string& FileName, StringHash& words ){
    string wordbuf;
    int no_words=0;
    ifstream wordfile( FileName.c_str(), ios::in);  
    while( wordfile >> wordbuf ) {
      words.Hash( wordbuf );
      ++no_words;
    }
    LOG << "  Read frequent words list from: " << FileName << " ("
	<< no_words << " words)." << endl;
  }
  
  bool TaggerClass::InitTagging( ){
    if ( !servermode ){
      if ( !cur_log->set_single_threaded_mode() ){
// 	LOG << "PROBLEM setting to single threaded Failed" << endl;
// 	LOG << "Tagging might be slower than hoped for" << endl;
      }
    }
    // read the lexicon
    //
    read_lexicon( MTLexFileName );
    //
    read_listfile( TopNFileName, kwordlist );
    
    nwords = 0;
    if ( TimblOptStr.empty() )
      Timbl_Options = "-FColumns ";
    else
      Timbl_Options = TimblOptStr;

    // we want Timbl to run silently ALWAYS
    // so overrule all -vS settings from old versions
    // smart users still can circumvent this ;)
    string::size_type pos = Timbl_Options.find( "-vS" );
    while ( pos != string::npos ){
      Timbl_Options[pos] = '+';
      pos = Timbl_Options.find( "-vS", pos+1 );
    }
    splits( Timbl_Options, knownstr, unknownstr, kwf, uwf );
    if( !knowntreeflag ){
      cerr << "<knowntreefile> not specified" << endl;
      return false;
    }
    else if( !unknowntreeflag){
      cerr << "<unknowntreefile> not specified" << endl;
      return false;
    }
    KnownTree = new TimblAPI( knownstr );
    if ( !KnownTree->Valid() )
      return false;
    unKnownTree = new TimblAPI( unknownstr );
    if ( !unKnownTree->Valid() )
      return false;
    // read a previously stored InstanceBase for known words
    //
    LOG << "  Reading case-base for known words from: " << KnownTreeName 
	<< "... " << endl;
    if ( !KnownTree->GetInstanceBase( KnownTreeName) ){
      cerr << "Could not read the known tree from " 
	   << KnownTreeName << endl;
      return false;
    }
    else {
      if ( !kwf.empty() ){
	if ( !KnownTree->GetWeights( kwf ) ){
	  cerr << "Couldn't read known weights from " << kwf << endl;
	  return false;
	}
	else
	  cerr << "\n  Read known weights from " << kwf << endl;
      }
      LOG << "  case-base for known words read." << endl;
      // read  a previously stored InstanceBase for unknown words
      //
      LOG << "  Reading case-base for unknown words from: " 
	  << UnknownTreeName << "... " << endl;
      if( !unKnownTree->GetInstanceBase( UnknownTreeName) ){
	LOG << "Could not read the unknown tree from " 
	     << UnknownTreeName << endl;
	return false;
      }
      else {
	if ( !uwf.empty() ){
	  if ( !unKnownTree->GetWeights( uwf ) ){
	    cerr << "Couldn't read unknown weights from " << uwf << endl;
	    return false;
	  }
	  else
	    LOG << "\n  Read unknown weights from " << uwf << endl;
	}
	LOG << "  case-base for unknown word read" << endl;
	string kfn_template = "/tmp/knownXXXXXX";
	string ufn_template = "/tmp/unknownXXXXXX";
	char *tfn = strdup( kfn_template.c_str() );
	int fd = mkstemp( tfn );
	string kTempFileName = tfn;
	if (fd==-1) {
	  LOG << "Could not open temporaryfile: " << kTempFileName 
	      << " (" << strerror(errno) << ")" << endl;
	  LOG << "this might not be a problem, so we go on..." << endl;
	}
	else {
	  close( fd );
	  unlink( tfn );
	  free( tfn );
	  KnownTree->WriteNamesFile( kTempFileName );
	  tfn = strdup( ufn_template.c_str() );
	  fd = mkstemp( tfn );
	  string uTempFileName = tfn;
	  if ( fd == -1 ){
	    LOG << "Could not open temporaryfile: " << uTempFileName 
		<< " (" << strerror(errno) << ")" << endl;
	    LOG << "this might not be a problem, so we go on..." << endl;
	    unlink( kTempFileName.c_str() );
	  }
	  else {
	    close( fd );
	    unlink( tfn );
	    free( tfn );
	    unKnownTree->WriteNamesFile( uTempFileName );
	    if ( old_style( uTempFileName ) 
		 || old_style( kTempFileName ) ){
	      LOG << "Timbl InstanceBases seem to be in old style format" 
		  << endl;
	      LOG << "please consider converting them" << endl;
	      if ( !r_option_name.empty() ){
		LOG << "use 'convert " << r_option_name << " " << KnownTreeName
		     << "'" << endl;
		LOG << "and 'convert " << r_option_name << " " << UnknownTreeName
		    << "'" << endl;
	      }
	      LOG << "unable to proceed" << endl;
	      unlink( uTempFileName.c_str() );
	      unlink( kTempFileName.c_str() );
	      exit(EXIT_FAILURE);
	    }
	    else {
	      unlink( kTempFileName.c_str() );
	      unlink( uTempFileName.c_str() );
	    }
	  }
	}
      }
    }
    LOG << "  Sentence delimiter set to '" << EosMark << "'" << endl;
    LOG << "  Beam size = " << Beam_Size << endl;    
    LOG << "  Known Tree, Algorithm = " 
	<< to_string( KnownTree->Algo() ) << endl;
    LOG << "  Unknown Tree, Algorithm = " 
	<< to_string( unKnownTree->Algo() ) << endl << endl;
    // the testpattern is of the form given in Ktemplate and Utemplate
    // here we allocate enough space for the larger of them to serve both
    //
    TestPat->reserve(max(Ktemplate.totalslots(),Utemplate.totalslots()));
    initialized = true;
    return true;
  }

  TaggerClass *TaggerClass::clone( Socket *sock ){
    TaggerClass *ta = new TaggerClass( *this );
    ta->Sock = sock;
    ta->mySentence = new sentence(); // we need our own testing structures
    ta->TestPat = new vector<int>;
    ta->TestPat->reserve(max(Ktemplate.totalslots(),Utemplate.totalslots()));
    ta->Beam = NULL; // own Beaming data
    ta->TheLex = new StringHash();
    return ta;
  }
  
  int TaggerClass::Run(){
    int result = -1;
    if ( initialized ){
      ostream *os;
      if ( OutputFileName != "" ){
	os = new ofstream( OutputFileName.c_str() );
      }
      else
	os = &cout;
      ifstream infile;    
      if ( !piped_input ){
	infile.open(TestFileName.c_str(), ios::in);
	if( infile.bad( )){
	  cerr << "Cannot read from " << TestFileName << endl;
	  result = 0;
	}
	else {
	  cerr << "Processing data from the file " << TestFileName 
	       << ":" <<  endl;
	  result = ProcessFile(infile, *os );
	}
      }
      else {
	cerr << "Processing data from the standard input" << endl;
	if ( input_kind == ENRICHED ){
	  cerr << "Enriched Inputformat not supported for stdin, sorry"
	       << endl;
	}
	else
	  result = ProcessFile( cin, *os );
      }
      if ( OutputFileName != "" ){
	delete os;
      }
    }
    return result;
  }

#if defined(PTHREADS)  
  static pthread_mutex_t known_lock = PTHREAD_MUTEX_INITIALIZER;
  static pthread_mutex_t unknown_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

  void TaggerClass::InitTest( MatchAction Action ){
    int nslots;
    // Now make a testpattern for Timbl to process.
    if ( Action == Unknown )
      nslots = Utemplate.totalslots() - Utemplate.skipfocus;
    else
      nslots = Ktemplate.totalslots() - Ktemplate.skipfocus;
    string teststring = pat_to_string( nslots, 0 );
    //    cerr << "test string = " << teststring << endl;
    const ValueDistribution *distribution;
    const TargetValue *answer;
    double distance;
    if ( Action == Known ){
#if defined(PTHREADS)
      pthread_mutex_lock( &known_lock );
#endif
      answer = KnownTree->Classify( teststring, distribution, distance );
#if defined(PTHREADS)
      pthread_mutex_unlock( &known_lock );
#endif
    }
    else {
#if defined(PTHREADS)
      pthread_mutex_lock( &unknown_lock );
#endif
      answer = unKnownTree->Classify( teststring, distribution, distance );
#if defined(PTHREADS)
      pthread_mutex_unlock( &unknown_lock );
#endif
    }
    if ( !answer ){
      cerr << "A classifying problem prevented continuing. Sorry!"
	   << endl;
      exit(EXIT_FAILURE);
    }
    else {
      distance_array.resize( mySentence->No_Words() );
      distribution_array.resize( mySentence->No_Words() );
      if ( distance_flag )
	distance_array[0] = distance;
      if ( distrib_flag )
	distribution_array[0] = distribution->DistToString();
      if ( IsActive( DBG ) ){
	LOG << "BeamData::InitPaths( "; mySentence->print(LOG); 
	LOG << " , " << answer << " , " << distribution << " )" << endl;
      }
      Beam->InitPaths( *TheLex, answer, distribution );
      if ( IsActive( DBG ) ){
	Beam->Print( LOG, 0, *TheLex );
      }
    }
  }
  
  
  bool TaggerClass::NextBest( int i_word, int beam_cnt ){
    MatchAction Action = Unknown;
    if ( Beam->paths[beam_cnt][i_word-1] == EMPTY_PATH ){
      return false;
    }
    else if ( mySentence->nextpat( &Action, *TestPat,
				   kwordlist, *TheLex,
				   i_word, Beam->paths[beam_cnt] ) ){
      // Now make a testpattern for Timbl to process.
      int nslots;
      if ( Action == Unknown )
	nslots = Utemplate.totalslots() - Utemplate.skipfocus;
      else 
	nslots = Ktemplate.totalslots() - Ktemplate.skipfocus;
      string teststring = pat_to_string( nslots, i_word );
      // process teststring to predict a category, using the 
      // appropriate tree
      //
      //      cerr << "teststring '" << teststring << "'" << endl;
      const TargetValue *answer;
      const ValueDistribution *distribution;
      double distance;
      if ( Action == Known ){
#if defined(PTHREADS)
	pthread_mutex_lock( &known_lock );
#endif
	answer = KnownTree->Classify( teststring, distribution, distance );
#if defined(PTHREADS)
	pthread_mutex_unlock( &known_lock );
#endif
      }
      else {
#if defined(PTHREADS)
	pthread_mutex_lock( &unknown_lock );
#endif
	answer = unKnownTree->Classify( teststring, distribution, distance );
#if defined(PTHREADS)
	pthread_mutex_unlock( &unknown_lock );
#endif
      }
      if ( !answer ){
	cerr << "A classifying problem prevented continuing. Sorry!"
	     << endl;
	exit(EXIT_FAILURE);
	return false;
      }
      else {
	if ( beam_cnt == 0 ){
	  if ( distance_flag )
	    distance_array[i_word] = distance;
	  if ( distrib_flag )
	    distribution_array[i_word] = distribution->DistToString();
	}
	Beam->NextPath( *TheLex, answer, distribution, beam_cnt ); 
	if ( IsActive( DBG ) )
	  Beam->PrintBest( LOG, *TheLex );
      }
      return true;
    }
    else {
      cerr << "skipping to next sentence" << endl;
      return false;
    }
  }

  string Tag( TaggerClass *tagger, const string& inp ){
    tagger->mySentence->reset( tagger->EosMark );
    tagger->mySentence->Fill( inp, false );
    string tag;
    if ( tagger )
      tagger->Tag( tag );
    return tag;
  }

  bool TaggerClass::Tag( string& tag ){
    tag = "";
    if ( !initialized ||
	 !InitBeaming( mySentence->No_Words() ) ){
      cerr << "ouch" << initialized << endl;
      return false;
    }
    mySentence->print(DBG);
    
    if ( mySentence->init_windowing(&Ktemplate,&Utemplate, 
				    MT_lexicon, *TheLex ) ) {
      // here the word window is looked up in the dictionary and the values
      // of the features are stored in the testpattern
      MatchAction Action = Unknown;
      if ( mySentence->nextpat( &Action, *TestPat, 
				kwordlist, *TheLex,
				0 )){ 

	DBG << "Start: " << mySentence->getword( 0 ) << endl;
	InitTest( Action );
	for ( int iword=1; iword < mySentence->No_Words(); iword++ ){
	  // clear best_array
	  DBG << endl << "Next: " << mySentence->getword( iword ) << endl;
	  Beam->ClearBest();
	  for ( int beam_count=0; beam_count < Beam_Size; beam_count++ ){
	    if ( !NextBest( iword, beam_count ) )
	      break;
	  }
	  Beam->Shift( mySentence->No_Words(), iword );
	  if ( IsActive( DBG ) ){
	    LOG << "after shift:" << endl;
	    Beam->Print( LOG, iword, *TheLex );
	  }
	}
      } // end one sentence
      tag = get_result();
      return true;
    }
    else
      return false;
  }
  
  string TaggerClass::get_result(){
    string result;
    string tagstring;
    //now some output
    for ( int Wcnt=0; Wcnt < mySentence->No_Words(); ++Wcnt ){
      // lookup the assigned category
      tagstring = indexlex( Beam->paths[0][Wcnt], *TheLex );
      // now we do the appropriate output, depending on known/unknown
      // words and the availability of a "correct tag".
      //
      result += mySentence->getword(Wcnt);
      if ( mySentence->known(Wcnt) ){
	if ( input_kind == UNTAGGED )
	  result += "/";
	else
	  result += "\t/\t";
      }
      else {
	if ( input_kind == UNTAGGED )
	  result += "//";
	else
	  result += "\t//\t";
      }      
      // output the correct tag if possible
      //
      if ( input_kind == ENRICHED ){
	result = result + mySentence->getenr(Wcnt) + "\t" 
	  + mySentence->gettag(Wcnt) + "\t" + tagstring;
	if ( distance_flag )
	  result += " " + toString( distance_array[Wcnt] );
	if ( distrib_flag )
	  result += " " + distribution_array[Wcnt];
	result += "\n";
      }
      else if ( input_kind == TAGGED ){
	result = result + mySentence->gettag(Wcnt) + "\t" + tagstring;
	if ( distance_flag )
	  result += " " + toString( distance_array[Wcnt] );
	if ( distrib_flag )
	  result += " " + distribution_array[Wcnt];
	result += "\n";
      }
      else
	result = result + tagstring + " ";
    } // end of output loop through one sentence
    if ( input_kind != ENRICHED )
      result = result + mySentence->Eos() + "\n";
    return result;
  }
  
  void TaggerClass::statistics( int& no_known, int& no_unknown,
				int& no_correct_known, 
				int& no_correct_unknown ){
    string result;
    string tagstring;
    //now some output
    for ( int Wcnt=0; Wcnt < mySentence->No_Words(); Wcnt++ ){
      tagstring = indexlex( Beam->paths[0][Wcnt], *TheLex );
      if ( mySentence->known(Wcnt) ){
	no_known++;
	if ( input_kind != UNTAGGED ){
	  if ( mySentence->gettag(Wcnt) == tagstring )
	    no_correct_known++;
	}
      }
      else {
	no_unknown++;
	if ( input_kind != UNTAGGED ){
	  if ( mySentence->gettag(Wcnt) == tagstring )
	    no_correct_unknown++;
	}
      }
    } // end of output loop through one sentence
  }
  
  int TaggerClass::ProcessFile( istream& infile, ostream& outfile ){
    bool go_on = true;
    int no_words=0;
    int no_correct_known=0;
    int no_correct_unknown=0;
    int no_known=0;
    int no_unknown=0;
    
    // loop as long as you get sentences
    //
    int HartBeat = 0;
    while ( go_on && 
	    (mySentence->reset( EosMark), mySentence->read(infile, input_kind ) ) ){
      if ( mySentence->No_Words() == 0 )
	continue;
      string tagged_sentence;
      if ( ++HartBeat % 100 == 0 ) {
	cerr << "."; cerr.flush();
      }
      if ( mySentence->getword(0) == EosMark ){
	// only possible for ENRICHED!
	outfile << EosMark << endl;
	continue;
      }
      if ( Tag( tagged_sentence ) ){
	// show the results of 1 sentence
	statistics( no_known, no_unknown,
		    no_correct_known, 
		    no_correct_unknown );
	outfile << tagged_sentence;
	// increase the counter of processed words
	no_words += mySentence->No_Words();
      }
      else {
	  // probably empty sentence??
      }
    } // end of while looping over sentences
    
    cerr << endl << endl << "Done: " << no_words 
	 << " words processed." << endl << endl;
    if ( no_words > 0 ){
      if ( input_kind != UNTAGGED ){
	cerr << "Classification Statistics:" << endl;
	cerr << endl << "  Known Words:" << endl;
	KnownTree->ShowStatistics(cerr);
	cerr << endl << "  UnKnown Words:" << endl;
	unKnownTree->ShowStatistics(cerr);
	cerr << endl 
	     << "  Total        : " << no_correct_known+no_correct_unknown 
	     << "\tcorrect from " << no_known+no_unknown << " ("
	     << ((float)(no_correct_known+no_correct_unknown) /
		 (float)(no_known+no_unknown))*100 
	     << " %)" << endl;
      }
      else {
	cerr << "  Known   words: " << no_known << endl;
	cerr << "  Unknown words: " << no_unknown;
	if ( no_unknown > 0 )
	  cerr << " ("
	       << ((float)(no_unknown)/(float)(no_unknown+no_known))*100
	       << " %)";
	cerr << endl;
	cerr << "  Total        : " << no_known+no_unknown << endl;
      }
    }
    return no_words;
  }
  
  
//**** stuff to process commandline options *********************************

  // used to convert relative paths to absolute paths

  /**
   * If you do 'Mbt -s some-path/xxx.settingsfile' Timbl can not find the 
   * tree files.
   *
   * Because the settings file can contain relative paths for files these 
   * paths are converted to absolute paths. 
   * The relative paths are taken relative to the pos ition of the settings
   * file, so the path of the settings file is prefixed to the realtive path.
   *
   * Paths that do not begin with '/' and do not have as second character ':'
   *      (C: or X: in windows cygwin) are considered to be relative paths
   */
  
  void prefixWithAbsolutePath( string& fileName, 
			       const string& prefix ) {
    //    cout << fileName << endl;
    if ( ( fileName[0] != '/' && fileName[1] != ':' )
	 && !( fileName[0]== '.' && fileName[1] == '/' ) ){
      fileName = prefix + fileName;
    }
    //    cout << fileName << endl;
  }
  
  bool TaggerClass::readsettings( string& fname ){
    ifstream setfile( fname.c_str(), ios::in);
    if(setfile.bad()){
      return false;
    }
    char SetBuffer[512];
    char value[512];
    while(setfile.getline(SetBuffer,511,'\n')){
      switch (SetBuffer[0]) {
      case 'B':
	if ( sscanf(SetBuffer,"B %d", &Beam_Size ) != 1 )
	  Beam_Size = 1;
	break;
      case 'd':
	dumpflag=true;
	cerr << "  Dumpflag ON" << endl;
	break;
      case 'C': {
	sscanf( SetBuffer, "C %s", value );
	Max_Conn_Str = value;
	cerr << "  Maximum number of connections  set to '" 
	     << Max_Conn_Str << "'" << endl;
	break;
      }
      case 'e': {
	sscanf( SetBuffer, "e %s", value );
	EosMark = value;
	break;
      }
      case 'k':
	sscanf(SetBuffer,"k %s", value );
	KnownTreeName = value;
	prefixWithAbsolutePath( KnownTreeName, SettingsFilePath );
	knowntreeflag = true; // there is a knowntreefile specified
	break;
      case 'l':
	sscanf(SetBuffer,"l %s", value );
	l_option_name = value;
	prefixWithAbsolutePath(l_option_name, SettingsFilePath );
	lexflag = true; // there is a lexicon specified
	break;
      case 'L':
	sscanf(SetBuffer,"L %s", value );
	L_option_name = value;
	prefixWithAbsolutePath(L_option_name, SettingsFilePath );
	klistflag = true;
	break; 
      case 'o':
	sscanf(SetBuffer,"t %s", value );
	OutputFileName = value;
	prefixWithAbsolutePath(OutputFileName, SettingsFilePath );
	break;
      case 'O':  // Option string for Timbl
	TimblOptStr = string(SetBuffer+1);
	break;
      case 'p':  // windowing pattern for known words
	KtmplStr = string( SetBuffer+2 );
	break;
      case 'P':  // windowing pattern for unknown words
	UtmplStr = string( SetBuffer+2 );
	break;
      case 'r':
	sscanf(SetBuffer,"r %s", value );
	r_option_name = value;
	prefixWithAbsolutePath(r_option_name, SettingsFilePath );
	reverseflag = true;
	break;
      case 'S':
#if defined( PTHREADS)
	sscanf( SetBuffer, "S %s", value );
	portnumstr = value;
	servermode = true; // We're going into Server MODE!
#else
	cerr << "Server mode NOT supported in this version!\n"
	     << "You must rebuild Mbt with MAKE_SERVER=YES in the Makefile\n"
	     << "sorry..." << endl;
	exit(EXIT_FAILURE);
#endif
	break;
      case 't':
	sscanf(SetBuffer,"t %s", value );
	TestFileName = value;
	prefixWithAbsolutePath(TestFileName, SettingsFilePath );
	piped_input = false; // there is a test file specified
	break;
      case 'E':
	if ( SetBuffer[1] == ' ' && sscanf(SetBuffer,"E %s", value ) > 0 ){
	  TestFileName = value;
	  prefixWithAbsolutePath(TestFileName, SettingsFilePath );
	  piped_input = false;
	  input_kind = ENRICHED; // an enriched tagged test file specified
	}
	else if ( !strncmp( SetBuffer, "ENRICHED", 8 ) )
	  input_kind = ENRICHED; // an enriched tagged test file specified
	else {
	  cerr << "Unknown option in settingsfile, ("
	       << SetBuffer << "), ignored." <<endl;
	  break;
	}
	break;
      case 'T':
	sscanf(SetBuffer,"T %s", value );
	TestFileName = value;
	prefixWithAbsolutePath(TestFileName, SettingsFilePath );
	piped_input = false;
	input_kind = TAGGED; // there is a tagged test file specified
	break;
      case 'u':
	sscanf(SetBuffer,"u %s", value );
	UnknownTreeName = value;
	prefixWithAbsolutePath(UnknownTreeName, SettingsFilePath );
	unknowntreeflag = true; // there is a unknowntreefile file specified
	break;
      default:
	cerr << "Unknown option in settingsfile, ("
	     << SetBuffer << "), ignored." <<endl;
	break;
      }
    }
    return true;
  }

  void TaggerClass::parse_run_args( TimblOpts& Opts ){
    string value;
    bool mood;
    if ( Opts.Find( 's', value, mood ) ){
      // if a settingsfile option has been given, read that first
      // and then override with commandline options 
      //
      SettingsFileName = value;
      // extract the absolute path to the settingsfile
      string::size_type lastSlash = SettingsFileName.rfind('/');
      if ( lastSlash != string::npos )
	SettingsFilePath = SettingsFileName.substr( 0, lastSlash+1 );
      else
	SettingsFilePath = "";
      // cout << SettingsFilePath << endl;
      if( !readsettings( SettingsFileName ) ){
	cerr << "Cannot read settingsfile " << SettingsFileName << endl;
	exit(EXIT_FAILURE);
      }
      Opts.Delete( 's' );
    };
    if ( Opts.Find( string("logfile"), value, mood ) ){
      logFile = value;
      Opts.Delete( string("logfile") );
    };
    if ( Opts.Find( 'B', value, mood ) ){
      int dum_beam = stringTo<int>(value);
      if (dum_beam>1)
	Beam_Size = dum_beam;
      else
	Beam_Size = 1;
      Opts.Delete( 'B' );
    };
    if ( Opts.Find( 'C', value, mood ) ){
      Max_Conn_Str = value;
      Opts.Delete( 'C' );
    };
    if ( Opts.Find( 'd', value, mood ) ){
      dumpflag=true;
      cerr << "  Dumpflag ON" << endl;
      Opts.Delete( 'd' );
    }
    if ( Opts.Find( 'D', value, mood ) ){
      if ( value == "LogNormal" )
	cur_log->setlevel( LogNormal );
      else if ( value == "LogDebug" )
	cur_log->setlevel( LogDebug );
      else if ( value == "LogHeavy" )
	cur_log->setlevel( LogHeavy );
      else if ( value == "LogExtreme" )
	cur_log->setlevel( LogExtreme );
      else {
	cerr << "Unknown Debug mode! (-D " << value << ")" << endl;
      }
      Opts.Delete( 'D' );
    }
    if ( Opts.Find( 'e', value, mood ) ){
      EosMark = value;
      Opts.Delete( 'e' );
    }
    if ( Opts.Find( 'k', value, mood ) ){
      KnownTreeName = value;
      knowntreeflag = true; // there is a knowntreefile specified
      Opts.Delete( 'k' );
    };
    if ( Opts.Find( 'l', value, mood ) ){
      l_option_name = value;
      lexflag = true; // there is a lexicon specified
      Opts.Delete( 'l' );
    };
    if ( Opts.Find( 'L', value, mood ) ){
      L_option_name = value;
      klistflag = true;
      Opts.Delete( 'L' );
    };
    if ( Opts.Find( 'o', value, mood ) ){
      OutputFileName = value;
      Opts.Delete( 'o' );
    };
    if ( Opts.Find( 'O', value, mood ) ){  // Option string for Timbl
      TimblOptStr = value;
      Opts.Delete( 'O' );
    };
    if ( Opts.Find( "pidfile", value, mood ) ){
      pidFile = value;
      Opts.Delete( "pidfile" );
    };
    if ( Opts.Find( 'r', value, mood ) ){
      r_option_name = value;
      reverseflag = true;
      Opts.Delete( 'r' );
    }
    if ( Opts.Find( 'S', value, mood ) ){
#if defined(PTHREADS)
      portnumstr = value;
      servermode = true;
#else
      cerr << "Server mode NOT supported in this version!\n"
	   << "You must rebuild Mbt with MAKE_SERVER=YES in the Makefile\n"
	   << "sorry..." << endl;
      exit(EXIT_FAILURE);
#endif
      Opts.Delete( 'S' );
    };
    if ( Opts.Find( 't', value, mood ) ){
      TestFileName = value;
      piped_input = false; // there is a test file specified
      Opts.Delete( 't' );
    };
    if ( Opts.Find( 'E', value, mood ) ){
      TestFileName = value;
      piped_input = false;
      input_kind = ENRICHED; // enriched tagged test file specified
      Opts.Delete( 'E' );
    };
    if ( Opts.Find( 'T', value, mood ) ){
      TestFileName = value;
      piped_input = false;
      if ( input_kind == ENRICHED ){
	cerr << "Option -T conflicts with ENRICHED format from settingsfile "
	     << "unable to continue" << endl;
	exit(EXIT_FAILURE);
      }
      input_kind = TAGGED; // there is a tagged test file specified
      Opts.Delete( 'T' );
    };
    if ( Opts.Find( 'u', value, mood ) ){
      UnknownTreeName = value;
      unknowntreeflag = true; // there is a unknowntreefile file specified
      Opts.Delete( 'u' );
    }
    if ( Opts.Find( 'v', value, mood ) ){
      vector<string> opts;
      size_t num = split_at( value, opts, "+" );
      for ( size_t i = 0; i < num; ++i ){
	if ( opts[i] == "di" )
	  distance_flag = true;
	if ( opts[i] == "db" )
	  distrib_flag = true;
      }
      Opts.Delete( 'v' );
    };
    if ( servermode && input_kind == ENRICHED ){
      cerr << "Servermode doesn't support enriched inputformat!" << endl
	   << "bailing out, sorry " << endl;
      exit(EXIT_FAILURE);
    }
  }
  
  TaggerClass *CreateTagger( TimblOpts& Opts ){
    TaggerClass *tagger = new TaggerClass;
    tagger->parse_run_args( Opts );
    tagger->set_default_filenames();
    tagger->InitTagging();
    return tagger;
  }

  void RemoveTagger( TaggerClass* tagger ){
    delete tagger;
  }

  int RunTagger( TimblOpts& Opts ){
    TaggerClass tagger;
    tagger.parse_run_args( Opts );
    tagger.set_default_filenames();
    tagger.InitTagging();
    int result = -1;
    if ( tagger.ServerMode() )
      result = tagger.RunServer();
    else {
      result = tagger.Run();
    }
    return result;
  }

  bool TaggerClass::InitLearning( ){
    // if not supplied on command line, make a default
    // name for both output files (conactenation of datafile 
    // and pattern string
    //
    create_lexicons( TestFileName );
    if ( TimblOptStr.empty() )
      Timbl_Options = "-a IB1";
    else
      Timbl_Options = TimblOptStr;
    splits( Timbl_Options, knownstr, unknownstr, kwf, uwf );
    // the testpattern is of the form given in Ktemplate and Utemplate
    // here we allocate enough space for the larger of them to serve both
    //
    TestPat->reserve(max(Ktemplate.totalslots(),Utemplate.totalslots()));
    return true;
  }
  
  int TaggerClass::makedataset( istream& infile, bool do_known ){    
    int no_words=0;
    int no_sentences=0;
    int nslots=0;
    ofstream outfile;
    MatchAction Action;
    if( do_known ){
      nslots = Ktemplate.totalslots() - Ktemplate.skipfocus;
      outfile.open( K_option_name.c_str(), ios::trunc | ios::out );
      Action = MakeKnown;
    }
    else {
      nslots = Utemplate.totalslots() - Utemplate.skipfocus;
      outfile.open( U_option_name.c_str(), ios::trunc | ios::out );
      Action = MakeUnknown;
    }
    // loop as long as you get sentences
    //
    int HartBeat = 0;
    while ( (mySentence->reset( EosMark), mySentence->read( infile, input_kind ) )){
      if ( mySentence->No_Words() == 0 )
	continue;
      //      mySentence->print( cerr );
      if ( mySentence->getword(0) == EosMark ){
	// only possible for ENRICHED!
	continue;
      }
      if ( HartBeat++ % 100 == 0 ){
	cout << "."; cout.flush();
      }
      if ( mySentence->init_windowing( &Ktemplate,&Utemplate, 
				       MT_lexicon, *TheLex ) ) {
	// we initialize the windowing procedure, this entails lexical lookup
	// of the words in the dictionary and the values
	// of the features are stored in the testpattern
	int swcn = 0;
	int thisTagCode;
	while( mySentence->nextpat( &Action, *TestPat, 
				    kwordlist, *TheLex,
				    swcn ) ){ 
	  bool skip = false;
	  if ( DoNpax && !do_known ){
	    if((uwordlist.Lookup(mySentence->getword(swcn)))==0){
	      skip = true;
	    }
	  }
	  if ( !skip )
	    for( int f=0; f < nslots; f++){
	      outfile << indexlex( (*TestPat)[f], *TheLex ) << " ";
	    }
	  thisTagCode = TheLex->Hash( mySentence->gettag(swcn) );
	  if ( !skip ){
	    for (vector<string>::iterator it = mySentence->getWord(swcn)->extraFeatures.begin(); it != mySentence->getWord(swcn)->extraFeatures.end(); it++)
	      outfile << *it << " ";
	    outfile << mySentence->gettag( swcn ) << '\n';
	  }
	  mySentence->assign_tag(thisTagCode, swcn ); 
	  ++swcn;
	  ++no_words;
	}
	no_sentences++;
      }
    }
    cout << endl << "      ready: " << no_words << " words processed." 
	 << endl;
//      cout << "Output written to ";
//      if ( do_known )
//        cout << K_option_name << endl;
//      else 
//        cout << U_option_name << endl;
    return no_words;
  }
  
  int TaggerClass::CreateKnown(){
    int nwords = 0;
    if ( knowntemplateflag ){
      cout << endl << "  Create known words case base" << endl
	   << "    Timbl options: '" << knownstr << "'" << endl;
      TimblAPI *UKtree = new TimblAPI( knownstr );
      if ( !UKtree->Valid() )
	exit(EXIT_FAILURE);
      cout << "    Algorithm = " << to_string(UKtree->Algo())
	   << endl << endl;
      if ( !piped_input ){
	ifstream infile( TestFileName.c_str(), ios::in );
	if(infile.bad()){
	  cerr << "Cannot read from " << TestFileName << endl;
	  return 0;
	}
	cout << "    Processing data from the file " 
	     << TestFileName << "...";
	cout.flush();
	nwords = makedataset( infile, true );
      }
      else {
	cout << "Processing data from the standard input" << endl;
	nwords = makedataset( cin, true );
      }
      cout << "    Creating case base: " << KnownTreeName << endl;
      UKtree->Learn( K_option_name );
      //      UKtree->ShowSettings( cout );
      UKtree->WriteInstanceBase( KnownTreeName );
      if ( !kwf.empty() )
	UKtree->SaveWeights( kwf );
      delete UKtree;
      if ( !KeepIntermediateFiles ){
	unlink(K_option_name.c_str());
	cout << "    Deleted intermediate file: " << K_option_name << endl;
      }
    }
    return nwords;
  }

  int TaggerClass::CreateUnknown(){
    int nwords = 0;
    if ( unknowntemplateflag ){
      cout << endl << "  Create unknown words case base" << endl
	   << "    Timbl options: '" << unknownstr << "'" << endl;
      TimblAPI *UKtree = new TimblAPI( unknownstr );
      if ( !UKtree->Valid() )
	exit(EXIT_FAILURE);
      cout << "    Algorithm = " << to_string(UKtree->Algo()) 
	   << endl << endl;
      if ( !piped_input ){
	ifstream infile( TestFileName.c_str(), ios::in );
	if(infile.bad()){
	  cerr << "Cannot read from " << TestFileName << endl;
	  return 0;
	}
	cout << "    Processing data from the file "
	     << TestFileName << "...";
	cout.flush();
	nwords = makedataset( infile, false );
      }
      else {
	cout << "Processing data from the standard input" << endl;
	nwords = makedataset( cin, false );
      }
      cout << "    Creating case base: " << UnknownTreeName << endl;
      UKtree->Learn( U_option_name );
      //      UKtree->ShowSettings( cout );
      UKtree->WriteInstanceBase( UnknownTreeName );
      if ( !uwf.empty() )
	UKtree->SaveWeights( uwf );
      delete UKtree;
      if ( !KeepIntermediateFiles ){
	unlink(U_option_name.c_str());
	cout << "    Deleted intermediate file: " << U_option_name << endl;
      }
    }
    return nwords;
  }
  
  void TaggerClass::CreateSettingsFile(){
    if ( SettingsFileName.empty() ) {
      SettingsFileName = TestFileName + ".settings";
    }
    ofstream out_file;
    if ( ( out_file.open( SettingsFileName.c_str(), ios::out ), 
	   !out_file.good() ) ){
      cerr << "couldn't create Settings-File `"
	   << SettingsFileName << "'" << endl;
      exit(EXIT_FAILURE);
    }
    else {
      if ( input_kind == ENRICHED )
	out_file << "ENRICHED" << endl;
      out_file << "e " << EosMark << endl;
      out_file << "l " << MTLexFileName << endl;
      out_file << "k " << KnownTreeName << endl;
      out_file << "u " << UnknownTreeName << endl;
      out_file << "p " << KtmplStr << endl;
      out_file << "P " << UtmplStr << endl;
      out_file << "O " << Timbl_Options << endl;
      out_file << "L " << TopNFileName << endl;
      out_file.close();
      cout << endl << "  Created settings file '" 
	   << SettingsFileName << "'" << endl;
    }
  }

  //**** stuff to process commandline options *****************************
  
  void TaggerClass::parse_create_args( TimblOpts& Opts ){
    string value;
    bool mood;
    if ( Opts.Find( '%', value, mood ) ){
      FilterTreshold = stringTo<int>( value );
    }
    if ( Opts.Find( 'd', value, mood ) ){
      dumpflag=true;
      cout << "  Dumpflag ON" << endl;
    }
    if ( Opts.Find( 'e', value, mood ) ){
      EosMark = value;
      cout << "  Sentence delimiter set to '" << EosMark << "'" << endl;
    }
    if ( Opts.Find( 'K', value, mood ) ){
      K_option_name = value;
      knownoutfileflag = true; // there is a knownoutfile specified
    }
    if ( Opts.Find( 'k', value, mood ) ){
      KnownTreeName = value;
      knowntreeflag = true; // there is a knowntreefile specified
    }
    if ( Opts.Find( 'l', value, mood ) ){
      l_option_name = value;
      lexflag = true; // there is a lexicon specified
    }
    if ( Opts.Find( 'L', value, mood ) ){
      L_option_name = value; 
      klistflag = true;
    } 
    if ( Opts.Find( 'M', value, mood ) ){
      TopNumber = stringTo<int>(value);
    }
    if ( Opts.Find( 'n', value, mood ) ){
      Npax = stringTo<int>(value);
      if ( Npax == 0 )
	DoNpax = false;
    }
    if ( Opts.Find( 'O', value, mood ) ){
      TimblOptStr = value; // Option string for Timbl
    }
    if ( Opts.Find( 'p', value, mood ) ){
      KtmplStr = value;  // windowing pattern for known words
    }
    if ( Opts.Find( 'P', value, mood ) ){
      UtmplStr = value;  // windowing pattern for unknown words
    }
    if ( Opts.Find( 'r', value, mood ) ){
      r_option_name = value;
      reverseflag = true;
    }
    if ( Opts.Find( 's', value, mood ) ){
      // if a settingsfile option has been specified, use that name
      SettingsFileName = value;
    }
    if ( Opts.Find( 'E', value, mood ) ){
      TestFileName = value;
      piped_input = false;
      input_kind = ENRICHED; // an enriched tagged test file specified
    }
    if ( Opts.Find( 'T', value, mood ) ){
      TestFileName = value;
      piped_input = false;
      input_kind = TAGGED; // there is a tagged test file specified
    }
    if ( Opts.Find( 'u', value, mood ) ){
      UnknownTreeName = value;
      unknowntreeflag = true; // there is a unknowntreefile file specified
    }
    if ( Opts.Find( 'U', value, mood ) ){
      U_option_name = value;
      unknownoutfileflag = true; // there is a unknownoutfile specified
    }
    if ( Opts.Find( 'X', value, mood ) ){
      KeepIntermediateFiles = true;
    }
    if ( Opts.Find( 'D', value, mood ) ){
      if ( value == "LogNormal" )
	cur_log->setlevel( LogNormal );
      else if ( value == "LogDebug" )
	cur_log->setlevel( LogDebug );
      else if ( value == "LogHeavy" )
	cur_log->setlevel( LogHeavy );
      else if ( value == "LogExtreme" )
	cur_log->setlevel( LogExtreme );
      else {
	cerr << "Unknown Debug mode! (-D " << value << ")" << endl;
      }
      Opts.Delete( 'D' );
    }
  }
  
  int MakeTagger( TimblOpts& Opts ){
    TaggerClass tagger;
    tagger.parse_create_args( Opts );
    tagger.set_default_filenames();
    tagger.InitLearning();
    // process the test material
    // and do the timing
    int nwords = 0;
    nwords += tagger.CreateKnown();
    nwords += tagger.CreateUnknown();
    tagger.CreateSettingsFile();
    return nwords;
  }  

}
