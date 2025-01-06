/* Copyright (c) 2019 Joan Moreau <jom@grosjo.net>, see the included COPYING file */

static long fts_backend_xapian_current_time()
{
        struct timeval tp;
        gettimeofday(&tp, NULL);
        return tp.tv_sec * 1000 + tp.tv_usec / 1000;
}

static long fts_backend_xapian_get_free_memory(int verbose) // KB  
{
	char buffer[250];
	char *p;
	struct rlimit rl;
	rl.rlim_cur=0;
        if(getrlimit(RLIMIT_AS,&rl)!=0) syslog(LOG_WARNING,"FTS Xapian: Memory limit by GETRLIMIT error: %s",strerror(errno));
        long m,l = rl.rlim_cur;
	FILE *f;
	if(l<1)
	{
		if(verbose>1) syslog(LOG_WARNING,"FTS Xapian: Memory limit not available from getrlimit (probably vsz_limit not set");
#if defined(__FreeBSD__) || defined(__NetBSD__)
        	u_int page_size;
        	uint_size uint_size = sizeof(page_size);
        	sysctlbyname("vm.stats.vm.v_page_size", &page_size, &uint_size, NULL, 0);
        	struct vmtotal vmt;
        	size_t vmt_size = sizeof(vmt);
        	sysctlbyname("vm.vmtotal", &vmt, &vmt_size, NULL, 0);
        	m = vmt.t_free * page_size / 1024.0f;
#else
		f=fopen("/proc/meminfo","r");
		if(f==NULL) return -1024;
		m=0;
		while(!feof(f))
	        {
        	        if ( fgets (buffer , 200 , f) == NULL ) break;
			p = strstr(buffer,"MemAvailable:");
			if(p!=NULL)
                	{
                        	m=atol(p+13);
				break;
			}
		}
		fclose(f);
#endif
		if(verbose>1) syslog(LOG_WARNING,"FTS Xapian: Memory available from meminfo : %ld MB",(long)(m/1024.0));
	}
	else
	{
		l = l / 1024.0f;
		if(verbose>1) syslog(LOG_WARNING,"FTS Xapian: Memory limit detected at %ld MB",(long)(l/1024.0f));

	        long pid=getpid();
		sprintf(buffer,"/proc/%ld/status",pid);
        	f=fopen(buffer,"r");
        	long memused=0;
        	if(f != NULL)
        	{
                	while(!feof(f))
                	{
                        	if ( fgets (buffer , 100 , f) == NULL ) break;
                        	p = strstr(buffer,"VmSize:");
                        	if(p!=NULL)
                        	{
                        	        memused=atol(p+7);
					break;
                        	}
                	}
                	fclose(f);
                	if(verbose>1) syslog(LOG_WARNING,"FTS Xapian: Memory used %ld MB",(long)(memused/1024.0f));
        	}
        	else
        	{
        	        if(verbose>1) syslog(LOG_WARNING,"FTS Xapian: Memory used not available from %s", buffer);
        	        memused=-1;
		}
		m = l - memused;
	}
	if(verbose>1) syslog(LOG_WARNING,"FTS Xapian: Available memory %ld MB",long(m/1024.0f));
	return m;
}

static void fts_backend_xapian_icutostring(icu::UnicodeString *t, std::string &s)
{
	s.clear();
	t->toUTF8String(s);
}

static long fts_backend_xapian_icutochar_length(icu::UnicodeString *t)
{
        std::string s;
	s.clear();
        t->toUTF8String(s);
        return strlen(s.c_str());
}

static bool fts_backend_xapian_clean_accents(icu::UnicodeString *t)
{
        UErrorCode status = U_ZERO_ERROR;
        icu::Transliterator * accentsConverter = icu::Transliterator::createInstance("NFD; [:M:] Remove; NFC", UTRANS_FORWARD, status);
        if(U_FAILURE(status))
        {
                std::string s("FTS Xapian: Can not allocate ICU translator + FreeMem="+std::to_string(long(fts_backend_xapian_get_free_memory(0)/1024.0f))+"MB");
                syslog(LOG_ERR,"%s",s.c_str());
                accentsConverter = NULL;
                return false;
        }
        accentsConverter->transliterate(*t);
        delete(accentsConverter);
        return true;
}

static void fts_backend_xapian_trim(icu::UnicodeString *d)
{
        while(d->startsWith(CHAR_SPACE) || d->startsWith(CHAR_KEY))
        {
                d->remove(0,1);
        }
        while(d->endsWith(CHAR_SPACE) || d->endsWith(CHAR_KEY))
        {
                d->truncate(d->length()-1);
        }
}

static void fts_backend_xapian_clean(icu::UnicodeString *t)
{
	fts_backend_xapian_clean_accents(t);
	t->toLower();

        long k=CHARS_PB;
        while(k>0)
        {
        	t->findAndReplace(chars_pb[k-1],CHAR_KEY);
                k--;
        }

        k=CHARS_SEP;
        while(k>0)
        {
                t->findAndReplace(chars_sep[k-1],CHAR_SPACE);
                k--;
        }

	fts_backend_xapian_trim(t);
}

static void fts_backend_xapian_get_lock(struct xapian_fts_backend *backend, long verbose, const char *s)
{
	std::unique_lock<std::timed_mutex> *lck;
	lck = new std::unique_lock<std::timed_mutex>(backend->mutex,std::defer_lock);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        while(!(lck->try_lock_for(std::chrono::milliseconds(1000 + std::rand() % 1000))))
	{
        	if(verbose>1) 
		{
			std::string sl("FTS Xapian: Waiting unlock... (");
			sl.append(s);
			sl.append(")");
			syslog(LOG_INFO,"%s",sl.c_str());
                }
	}
#pragma GCC diagnostic pop
	if(verbose>1)
	{
		std::string sl("FTS Xapian: Got lock (");
		sl.append(s);
                sl.append(")");
                syslog(LOG_INFO,"%s",sl.c_str());
	}
	backend->mutex_t = lck;
}

static void fts_backend_xapian_release_lock(struct xapian_fts_backend *backend, long verbose, const char *s)
{
	if(verbose>1)
	{
		std::string sl("FTS Xapian: Releasing lock (");
		sl.append(s);
		sl.append(")");
		syslog(LOG_INFO,"%s",sl.c_str());
	}
	if(backend->mutex_t !=NULL)
	{
		std::unique_lock<std::timed_mutex> *lck = backend->mutex_t;
		backend->mutex_t= NULL;
		delete(lck);
	}
}

static int fts_backend_xapian_sqlite3_vector_int(void *data, int argc, char **argv, char **azColName)
{
        if (argc < 1) return -1;
        
	uint32_t uid = atol(argv[0]);
        std::vector<uint32_t> * uids = (std::vector<uint32_t> *) data;
        uids->push_back(uid);
        
	return 0;
}

static int fts_backend_xapian_sqlite3_vector_icu(void *data, int argc, char **argv, char **azColName)
{
        if (argc < 1) return -1;
                 
        icu::StringPiece sp(argv[0]);
	icu::UnicodeString * t =  new icu::UnicodeString(icu::UnicodeString::fromUTF8(sp));
        if(fts_xapian_settings.verbose>0) i_info("FTS Xapian: sqlite3_vector_string : Adding %s",argv[0]);
        std::vector<icu::UnicodeString *> * v = (std::vector<icu::UnicodeString *> *) data;
        v->push_back(t);
        return 0;
}


class XResultSet
{
	public:
		long size;
		Xapian::docid * data;

	XResultSet() { size=0; data=NULL; }
	~XResultSet() { if (size>0) { i_free(data); } }

	void add(Xapian::docid did)
	{
		if(data==NULL)
		{
			data=(Xapian::docid *)i_malloc(sizeof(Xapian::docid));
		}
		else
		{
			data=(Xapian::docid *)i_realloc(data,size*sizeof(Xapian::docid),(size+1)*sizeof(Xapian::docid));
		}
		data[size]=did;
		size++;
	}
};

class XQuerySet
{
	private:
		const char * header;
		icu::UnicodeString * text;
		XQuerySet ** qs;
		Xapian::Query::op global_op;
		bool item_neg; // for the term
		long qsize;

	public:
		long limit;

	XQuerySet()
	{
		qsize=0; qs=NULL;
		limit=2;
		header=NULL;
		text=NULL;
		global_op = Xapian::Query::op::OP_OR;
	}

	XQuerySet(Xapian::Query::op op, long l)
	{
		qsize=0; qs=NULL;
		limit=2;
		if(l>2) { limit=l; }
		header=NULL;
		text=NULL;
		global_op=op;
	}

	~XQuerySet()
	{
		if(text!=NULL) 
		{ 
			delete(text); 
			text=NULL; 
		}

		for(long j=0;j<qsize;j++)
		{
			delete(qs[j]);
		}
		if(qsize>0) free(qs);
		qsize=0; qs=NULL;
	}

	void add(long uid)
	{
		std::string s = std::to_string(uid);
		icu::UnicodeString t(s.c_str());
		add(hdrs_emails[0],&t,false);
	}
		
	void add(const char * h2, icu::UnicodeString *t, bool is_neg)
	{
		if(h2==NULL) return;
		if(t==NULL) return;

		icu::UnicodeString h(h2);
		h.trim();
		h.toLower();
                if(h.length()<1) return;

		long i,j,k;
		XQuerySet * q2;
		icu::UnicodeString *r;

		fts_backend_xapian_clean(t);
		if(t->length()<limit) return;
	
		i = t->lastIndexOf(CHAR_SPACE);
                if(i>0)
                {
			if(is_neg)
			{
                        	q2 = new XQuerySet(Xapian::Query::OP_AND_NOT,limit);
			}
			else
			{
				q2 = new XQuerySet(Xapian::Query::OP_AND,limit);
			}
                        while(i>0)
                        {
                                j = t->length();
                                r = new icu::UnicodeString(*t,i+1,j-i-1);
                                q2->add(h2,r,false);
                                delete(r);
                                t->truncate(i);
                                fts_backend_xapian_trim(t);
                                i = t->lastIndexOf(CHAR_SPACE);
                        }
                        q2->add(h2,t,false);
                        if(q2->count()>0) add(q2); else delete(q2);
                        return;
                }

		if(h.compare(XAPIAN_WILDCARD)==0)
		{
			if(is_neg)
			{
				q2 = new XQuerySet(Xapian::Query::OP_AND_NOT,limit);
			}
			else
			{
                        	q2 = new XQuerySet(Xapian::Query::OP_OR,limit);
			}
			for(i=1;i<HDRS_NB;i++)
			{
				if(i!=XAPIAN_EXPUNGE_HEADER) q2->add(hdrs_emails[i],t,false);
			}
			add(q2);
			return;
		}
                else
		{
                        i=0;
                        while((i<HDRS_NB) && (h.compare(hdrs_emails[i])!=0))
                        {
                                i++;
                        }
                        if(i>=HDRS_NB)
                        {
                                return;
                        }
			h2=hdrs_emails[i];
                }

		if(text==NULL)
		{
			text=new icu::UnicodeString(*t);
			header=h2;
			item_neg=is_neg;
			return;
		}

                q2 = new XQuerySet(Xapian::Query::OP_AND,limit);
		q2->add(h2,t,is_neg);
		add(q2);
	}

	void add(XQuerySet *q2)
	{
		if(qsize<1)
		{
			qs=(XQuerySet **)malloc(sizeof(XQuerySet*));
		}
		else
		{
			qs=(XQuerySet **)realloc(qs,(qsize+1)*sizeof(XQuerySet*));
		}
		qs[qsize]=q2;
		qsize++;
	}

	int count()
	{
		int c=0;
		if(text!=NULL) c=1;
		c+=qsize;
		return c;
	}

	std::string get_string()
	{
		std::string s("");

		if(count()<1) return s;

		if(text!=NULL)
		{
			if(item_neg) s.append("NOT ( ");
			s.append(header);
			s.append(":");
			s.append("\"");
			text->toUTF8String(s);
			s.append("\"");
			if(item_neg) s.append(")");
		}

		const char * op;
		switch(global_op)
		{
			case Xapian::Query::OP_OR : op=" OR "; break;
			case Xapian::Query::OP_AND : op=" AND "; break;
			case Xapian::Query::OP_AND_NOT : op=" AND NOT "; break;
			default : op=" ERROR ";
		}	

		for (int i=0;i<qsize;i++)
		{
			int c=qs[i]->count();
			if(c<1) continue;

			if(s.length()>0) s.append(op);

			if(c>1)
			{
				s.append("(");
				s.append(qs[i]->get_string());
				s.append(")");
			}
			else s.append(qs[i]->get_string());
		}
		return s;
	}

	Xapian::Query * get_query(Xapian::Database * db)
	{
		Xapian::Query * q = NULL;
		Xapian::Query *q2, *q3;

		if(text!=NULL)
                {
			std::string s(header);
                        s.append(":");
                        s.append("\"");
                        text->toUTF8String(s);
                        s.append("\"");

			Xapian::QueryParser * qp = new Xapian::QueryParser();
			for(int i=0; i< HDRS_NB; i++) qp->add_prefix(hdrs_emails[i], hdrs_xapian[i]);
			qp->set_database(*db);
			q = new Xapian::Query(qp->parse_query(s.c_str(),Xapian::QueryParser::FLAG_DEFAULT));
			delete (qp);
			if(item_neg)
			{
				q2 = new Xapian::Query(Xapian::Query::MatchAll);
				q3 = new Xapian::Query(Xapian::Query::OP_AND_NOT,*q2,*q);
				delete(q2);
				delete(q);
				q=q3;
			}
		}
		if(qsize<1)
		{
			if(q==NULL) q = new Xapian::Query(Xapian::Query::MatchNothing);
			return q;
                }

		if(q==NULL)
		{	
			q=qs[0]->get_query(db);
		}
		else
		{
			q2 = new Xapian::Query(global_op,*q,*(qs[0]->get_query(db)));
			delete(q);
			q=q2;
		}
		for (int i=1;i<qsize;i++)
		{
			q2 = new Xapian::Query(global_op,*q,*(qs[i]->get_query(db)));
			delete(q);
			q=q2;
		}
		return q;
	}
};

class XDoc
{
	private:
                std::vector<icu::UnicodeString *> * terms;
		std::vector<icu::UnicodeString *> * strings;
		std::vector<const char *> * headers;
		std::vector<icu::UnicodeString *> * dict;

	public:
		long uid;
		char * uterm;
		Xapian::Document * xdoc;
		long status;
		long status_n;
		long nterms,nlines,ndict;
 
        XDoc(long luid)
	{
		uid=luid;
                std::string s;
                s.append("Q"+std::to_string(uid));
                uterm = (char*)malloc((s.length()+1)*sizeof(char));
                strcpy(uterm,s.c_str());

		strings = new std::vector<icu::UnicodeString *>;
		strings->clear();
		headers = new std::vector<const char *>;
		headers->clear();
		terms = new std::vector<icu::UnicodeString *>;
		terms->clear();
		nterms=0; nlines=0; ndict=0;

		xdoc=NULL; dict=NULL;
		status=0; status_n=0;
	}

	~XDoc() 
	{
		for(icu::UnicodeString * t : *terms)
		{
			delete(t);
		}
		terms->clear(); delete(terms);
	
		headers->clear(); delete(headers);

		for(icu::UnicodeString * t : *strings)
		{
			delete(t);
		}
		strings->clear(); delete(strings);

		if(xdoc!=NULL) delete(xdoc);
		free(uterm);
	}

	std::string getDocSummary()
	{
		std::string s("Doc "); 
		s.append(std::to_string(uid));
		s.append(" uterm=");
		s.append(uterm);
		s.append(" #lines=" + std::to_string(nlines));
		s.append(" #terms=" + std::to_string(nterms));
		s.append(" #dict=" + std::to_string(ndict));
		s.append(" status=" + std::to_string(status));
		return s;
	}

	void raw_load(const char *h, const char *d, int32_t size, long verbose, const char * title)
	{
		icu::UnicodeString * t;
                {
                        icu::StringPiece sp(d,size);
                        t =  new icu::UnicodeString(icu::UnicodeString::fromUTF8(sp));
                }
                headers->push_back(h);
                strings->push_back(t);
		nlines++;
	}

        long terms_add(icu::UnicodeString * w,long pos, long l)
        {
                if(l==0)
                {
                        terms->insert(terms->begin()+pos,new icu::UnicodeString(*w));
			nterms++;
                        return pos;
                }

                long n = std::floor(l*0.5f);
                int c = terms->at(pos+n)->compare(*w);

                // If already exist, return
                if(c==0) return pos;

                // If middle pos is lower than d, search after pos+n
                if(c<0) return terms_add(w,pos+n+1,l-n-1);

                // All other case, search before
                return terms_add(w,pos,n);
        }

	void terms_push(const char *h, icu::UnicodeString *t)
	{
		fts_backend_xapian_trim(t);
		long n = t->length();
		
		if(n>=fts_xapian_settings.partial)
		{
			t->truncate(XAPIAN_TERM_SIZELIMIT-strlen(h)-1);	
			while(fts_backend_xapian_icutochar_length(t)>=XAPIAN_TERM_SIZELIMIT-strlen(h)-1)
			{
				t->truncate(t->length()-1);
			}
			dict_add(t,0,dict->size());
			t->insert(0,h);
			terms_add(t,0,terms->size());
                }
                delete(t);
	}

	void dict_set(std::vector<icu::UnicodeString *> * d)
	{
		dict = d;
	}

        long dict_add(icu::UnicodeString * w,long pos, long l)
        {
                if(l==0)
                {
                        dict->insert(dict->begin()+pos,new icu::UnicodeString(*w));
			ndict++;
                        return pos;
                }

                long n = std::floor(l*0.5f);
                int c = dict->at(pos+n)->compare(*w);

                // If already exist, return
                if(c==0) return pos;

                // If middle pos is lower than d, search after pos+n
                if(c>0) return dict_add(w,pos+n+1,l-n-1);

                // All other case, search before
                return dict_add(w,pos,n);
        }

	bool terms_create(long verbose, const char * title)
	{
		icu::UnicodeString *t;
		const char * h;
		long k;
		
		while((terms->size()<XAPIAN_MAXTERMS_PERDOC) && (strings->size()>0))
		{
			h = headers->back(); headers->pop_back();
			t = strings->back(); strings->pop_back();
			
			fts_backend_xapian_clean(t);

                	k = t->lastIndexOf(CHAR_SPACE);
                	while(k>0)
                	{
                        	terms_push(h,new icu::UnicodeString(*t,k+1));
                        	t->truncate(k);
                        	fts_backend_xapian_trim(t);
                        	k = t->lastIndexOf(CHAR_SPACE);
			}
			terms_push(h,t);
                }
		return true;
	}

	bool doc_create(long verbose, const char * title)
	{
		if(verbose>0) syslog(LOG_INFO,"%s adding %ld terms",title,nterms);
		try
		{
			icu::UnicodeString *t;
			xdoc = new Xapian::Document();
			xdoc->add_value(1,Xapian::sortable_serialise(uid));
			xdoc->add_term(uterm);
			std::string s;
			long n = terms->size();
			while(n>0)
			{
				n--;
				t=terms->back();
				terms->pop_back();
				fts_backend_xapian_icutostring(t,s);
				if(verbose>1) syslog(LOG_INFO,"%s adding terms for (%s) : %s",title,uterm,s.c_str());
				xdoc->add_term(s.c_str());
				delete(t);
			}
		}
		catch(Xapian::Error e)
                {
			return false;
		}
		return true;
	} 
};

static void fts_backend_xapian_worker(void *p);

class XDocsWriter
{
	private:
		XDoc * doc;
		long verbose, lowmemory;
		std::thread *t;
		char * title;
		struct xapian_fts_backend *backend;
                std::vector<icu::UnicodeString *> * dict;
		int position;
	public:
		bool started,toclose,terminated;
		
	XDocsWriter(struct xapian_fts_backend *b, long n)
	{
		backend=b;
		std::string s;
                s.clear(); s.append("DW #"+std::to_string(n)+" (");
                s.append(backend->boxname);
                s.append(",");
                s.append(backend->xap_db);
                s.append(") - ");
                title=(char *)malloc((s.length()+1)*sizeof(char));
                strcpy(title,s.c_str());

		t=NULL;
		doc=NULL;
		toclose=false;
		terminated=false;
		started=false;
		verbose=fts_xapian_settings.verbose;
		lowmemory = fts_xapian_settings.lowmemory;

		dict = new std::vector<icu::UnicodeString *>;
                dict->clear();

		position=0;
	}

	bool checkDB()
	{
		if(backend->dbw != NULL) return true;
             
		backend->pending=0;
           
                try
                {
			if(verbose>0)
			{
				std::string s(title);
				s.append("Opening DB (RW)");
				syslog(LOG_INFO,"%s",s.c_str());
			}
			backend->dbw = new Xapian::WritableDatabase(backend->xap_db,Xapian::DB_CREATE_OR_OPEN | Xapian::DB_BACKEND_GLASS);
			return true;
		}
                catch(Xapian::DatabaseLockError e)
                {
			std::string s(title);
			s.append("Can't lock the DB : ");
			s.append(e.get_type());
			s.append(" - ");
			s.append(e.get_msg());
                        syslog(LOG_WARNING,"%s",s.c_str());
		}
                catch(Xapian::Error e)
                {
			std::string s(title);
			s.append("Can't open the DB RW : ");
                        s.append(e.get_type());
                        s.append(" - ");
                        s.append(e.get_msg());
                        syslog(LOG_WARNING,"%s",s.c_str());
                }
		return false;
	}

        void close()
        {
                toclose=true;
                if(t!=NULL)
                {
                        t->join();
                        delete(t);
                }
                t=NULL;
                terminated=true;
        }

	~XDocsWriter()
	{
		close();
		free(title);
                for(icu::UnicodeString * t : *dict)
                {
                        delete(t);
                }
                dict->clear(); delete(dict);

	}

	std::string getSummary()
	{
		std::string s(title);
		s.append(" position="+std::to_string(position));
		s.append(" queued_docs="+std::to_string(backend->docs.size()));
		s.append(" dict_size="+std::to_string(dict->size()));
		s.append(" terminated="+std::to_string(terminated));
		return s;
	}

	bool launch(const char * from)
	{
		if(verbose>0)
		{
			std::string s(title);
			s.append("Launching thread from ");
			s.append(from);
			syslog(LOG_INFO,"%s",s.c_str());
		}

		try
		{
			t = new std::thread(fts_backend_xapian_worker,this);
		}
		catch(std::exception e)
		{
			std::string s(title);
			s.append("Thread error ");
			s.append(e.what());
			syslog(LOG_ERR,"%s",s.c_str());
			t = NULL;
			return false;
		}
		started=true;
		return true;
	}

	long checkMemory()
	{
		std::string s;
		// Memory check
                long m = fts_backend_xapian_get_free_memory(verbose);
                if(verbose>0) { s=title; s.append("Memory : Free = "+std::to_string((long)(m / 1024.0f))+" MB vs limit = "+std::to_string(lowmemory)+" MB | Pendings in cache = "+std::to_string(backend->pending)+" / "+std::to_string(XAPIAN_WRITING_CACHE) + " | Dict size = "+std::to_string(dict->size())+" / "+std::to_string(XAPIAN_DICT_MAX)); syslog(LOG_WARNING,"%s",s.c_str()); }
                if((backend->dbw!=NULL) && ((backend->pending > XAPIAN_WRITING_CACHE) || (dict->size() > XAPIAN_DICT_MAX) || ((m>0) && (m<(lowmemory*1024))))) // too little memory or too many pendings
                {
			fts_backend_xapian_get_lock(backend, verbose, title);
			// Repeat test because the close may have happen in another thread
			if((backend->dbw!=NULL) && (backend->pending > 0))
			{
				try
                        	{
                        		s=title;
                        	        s.append("Committing "+std::to_string(backend->pending)+" docs due to low free memory ("+ std::to_string((long)(m/1024.0f))+" MB vs "+std::to_string(lowmemory)+" MB) or Cached docs ("+std::to_string(backend->pending)+") > "+std::to_string(XAPIAN_WRITING_CACHE));
                        	        syslog(LOG_WARNING,"%s",s.c_str());
                        	        backend->dbw->close();
                        	        delete(backend->dbw);
                        	        backend->dbw=NULL;
                        	        backend->pending = 0;
					dict_store();
                        	}
                        	catch(Xapian::Error e)
                        	{
                        		std::string s(title);
                        	        s.append("Can't commit DB1 : ");
                        	        s.append(e.get_type());
                        	        s.append(" - ");
                        	        s.append(e.get_msg());
                        	        syslog(LOG_ERR,"%s",s.c_str());
                        	}
                        	catch(std::exception e)
                        	{
                        		std::string s(title);
                        	        s.append("Can't commit DB2 : ");
                        	        s.append(e.what());
                        	        syslog(LOG_ERR,"%s",s.c_str());
                        	}
			}
			fts_backend_xapian_release_lock(backend, verbose, title);
		}
		return m;
	}
		
        void dict_store()
        {
                long m = dict->size();
                if(m<1) return;

		long t=fts_backend_xapian_current_time();
                if(verbose>0) syslog(LOG_INFO,"%sFlushing Dictionnary (%ld items)",title,dict->size());
                sqlite3 * db = NULL;
                if(sqlite3_open_v2(backend->dict_db,&db,SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READWRITE,NULL) != SQLITE_OK )
                {
                        syslog(LOG_ERR,"FTS Xapian: Can not open %s : %s",backend->dict_db,sqlite3_errmsg(db));
                        return;
                }
		char *zErrMsg = 0;
		std::string sql;
		long n=0;
                while(m>0)
		{
			sql.clear();
			dict->back()->toUTF8String(sql);
			sql=replaceDictWord + sql + "'," + std::to_string(sql.length()) + ")";
			dict->pop_back();
			m--;

			if(sqlite3_exec(db,sql.c_str(),NULL,0,&zErrMsg) != SQLITE_OK )
                	{
                        	syslog(LOG_ERR,"FTS Xapian: Can not replace keyword (%s) : %s",sql.c_str(),zErrMsg);
                        	sqlite3_free(zErrMsg);
                	}
			else n++;
                }
                sqlite3_close(db);
		if(verbose>0) syslog(LOG_INFO,"%sFlushing Dictionnary : %ld done in %ld msec",title,n, fts_backend_xapian_current_time()-t);
        }

	void worker()
	{
		long start_time = fts_backend_xapian_current_time();
		XDoc *doc = NULL;
		long totaldocs=0;
		long sl=0, dt=0;
		std::string s;
		dict->clear();

		while((!toclose) || (doc!=NULL))
		{
			if(doc==NULL)
			{
				if(verbose>0) { s=title; s.append("Searching doc"); syslog(LOG_INFO,"%s",s.c_str()); }

				position=1;
				fts_backend_xapian_get_lock(backend, verbose, title);
				position=2;
				if((backend->docs.size()>0) && (backend->docs.back()->status==1)) 
                        	{
					doc = backend->docs.back();
					backend->docs.pop_back();
					doc->dict_set(dict);
					dt=fts_backend_xapian_current_time();
				}
				fts_backend_xapian_release_lock(backend, verbose, title);
				position=3;
			}

			if(doc==NULL)
			{
				position=4;
				sl++;
				if((sl>50) && (verbose>0)) 
				{ 
					s=title; s.append("No-op"); syslog(LOG_INFO,"%s",s.c_str());
					sl=0;
				}
				std::this_thread::sleep_for(XAPIAN_SLEEP);
			}
			else if(doc->status==1)	
			{
				position=5;
				checkMemory();
				if(verbose>0) { s=title; s.append("Populating stems : "+doc->getDocSummary()); syslog(LOG_INFO,"%s",s.c_str()); }
				if(doc->terms_create(verbose,title)) 
				{ 
					position=6;
					doc->status=2; doc->status_n=0;
					dt=fts_backend_xapian_current_time()-dt;
					if(verbose>0) { s=title; s.append("Populating stems : "+doc->getDocSummary()+" done in " +std::to_string(dt)+" msec"); syslog(LOG_INFO,"%s",s.c_str()); }
					dt=fts_backend_xapian_current_time();
				}
				else 
				{
					doc->status_n++;
					if(verbose>0) { s=title; s.append("Populating stems : Error - "+doc->getDocSummary()); syslog(LOG_ERR,"%s",s.c_str()); }
					if(doc->status_n > XAPIAN_MAX_ERRORS) 
					{
						delete(doc);
						doc=NULL;
					}
				}
				position=7;
			}
			else if(doc->status==2)
			{
				position=8;
				checkMemory();
				if(verbose>0) { s=title; s.append("Creating Xapian doc : "+doc->getDocSummary()); syslog(LOG_INFO,"%s",s.c_str()); }
				if(doc->doc_create(verbose,s.c_str()))
				{
					position=9;
					doc->status=3;
					doc->status_n=0;
					dt=fts_backend_xapian_current_time()-dt;
					if(verbose>0) { s=title; s.append("Creating Xapian doc : "+doc->getDocSummary()+" done in " +std::to_string(dt)+" msec"); syslog(LOG_INFO,"%s",s.c_str()); }
					dt=fts_backend_xapian_current_time();
				}
				else
				{
					doc->status_n++;
					if(verbose>0) { s=title; s.append("Create document : Error"); syslog(LOG_INFO,"%s",s.c_str()); }  
                                        if(doc->status_n > XAPIAN_MAX_ERRORS)
                                        {
                                                delete(doc);
                                                doc=NULL;
                                        }
				}
				position=10;
			}
                        else
			{
				position=11;
				if(verbose>0) { s=title; s.append("Pushing : "+doc->getDocSummary()); syslog(LOG_INFO,"%s",s.c_str()); }
                        	if(doc->nterms > 0)
                        	{
					long m = checkMemory();
					fts_backend_xapian_get_lock(backend, verbose, title);
					position=12;
					if(checkDB())
					{
						position=13;
						try
                               	        	{
                               	                	backend->dbw->replace_document(doc->uterm,*(doc->xdoc));
							backend->pending++;
							backend->total_docs++;
							delete(doc);
							doc=NULL;
							if(verbose>0)
                                                        {
                                                                s=title;
                                                                s.append("Pushing done in "+std::to_string(fts_backend_xapian_current_time()-dt)+" msec");
                                                                syslog(LOG_INFO,"%s",s.c_str());
                                                        }
							totaldocs++;
                        	        	}
						catch(Xapian::Error e)
                               	                {
                               	                        s=title;
							s.append(" "+doc->getDocSummary());
                               	                        s.append("Can't write doc1 : ");
                               	                        s.append(e.get_type());
                               	                        s.append(" - ");
                               	                        s.append(e.get_msg());
                               	                        syslog(LOG_ERR,"%s",s.c_str());
                               	                }
                               	                catch(std::exception e)
                               	                {
                               	                        s=title;
                               	                        s.append("Can't write doc2");
                               	                        syslog(LOG_ERR,"%s",s.c_str());
                               	                }
					}
					position=14;
					fts_backend_xapian_release_lock(backend, verbose, title);	
					position=15;
				}
				else 
				{
					delete(doc);
					doc=NULL;
				}
                        }
                }

		position=16;
		fts_backend_xapian_get_lock(backend, verbose, title);
		position=17;
		dict_store();
		position=18;
		fts_backend_xapian_release_lock(backend, verbose, title);

		position=19;
		terminated=true;
                if(verbose>0) 
		{
			std::string s(title);
			s.append("Wrote "+std::to_string(totaldocs)+" within "+std::to_string(fts_backend_xapian_current_time() - start_time)+" msec");
			syslog(LOG_INFO,"%s",s.c_str());
		}
	}
};

static void fts_backend_xapian_worker(void *p)
{
	XDocsWriter *xw = (XDocsWriter *)p;
	xw->worker();
}
	
static bool fts_backend_xapian_open_readonly(struct xapian_fts_backend *backend, Xapian::Database ** dbr)
{
	if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: fts_backend_xapian_open_readonly");

	if((backend->xap_db == NULL) || (strlen(backend->xap_db)<1))
	{
		i_warning("FTS Xapian: Open DB Read Only : no DB name");
		return false;
	}

	try
	{
		if(fts_xapian_settings.verbose>0) i_info("FTS Xapian: Opening DB (RO) %s",backend->xap_db);
		*dbr = new Xapian::Database(backend->xap_db,Xapian::DB_CREATE_OR_OPEN | Xapian::DB_BACKEND_GLASS);
	}
	catch(Xapian::Error e)
	{
		i_error("FTS Xapian: Can not open RO index (%s) %s : %s - %s %s ",backend->boxname,backend->xap_db,e.get_type(),e.get_msg().c_str(),e.get_error_string());
		return false;
	}
	return true;
}

static void fts_backend_xapian_oldbox(struct xapian_fts_backend *backend)
{
	if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: fts_backend_xapian_oldbox");

	if(backend->old_guid != NULL)
	{
		/* Performance calculator*/
		long dt = fts_backend_xapian_current_time() - backend->start_time;
		double r=0;
		if(dt>0)
		{
			r=backend->total_docs*1000.0;
			r=r/dt;
		}
		/* End Performance calculator*/

		if(fts_xapian_settings.verbose>0) i_info("FTS Xapian: Done indexing '%s' (%s) (%ld msgs in %ld msec, rate: %.1f)",backend->old_boxname, backend->xap_db,backend->total_docs,dt,r);

		i_free(backend->old_guid); backend->old_guid = NULL;
		i_free(backend->old_boxname); backend->old_boxname = NULL;
	}

	if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: fts_backend_xapian_oldbox - done");
}

static void fts_backend_xapian_close_db(Xapian::WritableDatabase * dbw,const char * dbpath,const char * boxname, long verbose)
{
        long t;

	if(verbose>0)
	{
		t = fts_backend_xapian_current_time();
		syslog(LOG_INFO,"FTS Xapian : Closing DB (%s,%s)",boxname,dbpath);
	}
        try
        {
		dbw->close();
                delete(dbw);
	}
        catch(Xapian::Error e)
        {
                syslog(LOG_ERR, "FTS Xapian: Can't close Xapian DB (%s) %s : %s - %s %s",boxname,dbpath,e.get_type(),e.get_msg().c_str(),e.get_error_string());
        }
	catch(std::exception e)
        {
                syslog(LOG_ERR, "FTS Xapian : Closing db (%s) error %s",dbpath,e.what());
        }

	if(verbose>0) 
	{
		t = fts_backend_xapian_current_time()-t;
		syslog(LOG_INFO,"FTS Xapian : DB (%s,%s) closed in %ld ms",boxname,dbpath,t);
	}
}

static void fts_backend_xapian_close(struct xapian_fts_backend *backend, const char * reason)
{
	if(fts_xapian_settings.verbose>0) i_info("FTS Xapian : Closing all DWs (%s)",reason);
	
	fts_backend_xapian_get_lock(backend,fts_xapian_settings.verbose,reason);
	if((backend->docs.size()>0) && (backend->docs.front()->status<1)) backend->docs.front()->status=1;
        fts_backend_xapian_release_lock(backend,fts_xapian_settings.verbose,reason);

	long n=0;
	while(backend->docs.size()>0)
	{
		n++;
                if((n>50) and (fts_xapian_settings.verbose>0))
		{
			i_info("FTS Xapian: Waiting for all pending documents (%ld) to be processed (Sleep5) with %ld threads",backend->docs.size(),backend->threads.size());
			n=0;
		}
                std::this_thread::sleep_for(XAPIAN_SLEEP);
        }

	for(auto & xwr : backend->threads)
        {
		xwr->toclose=true;
	}

	XDocsWriter * xw;
	n=0;
	while(backend->threads.size()>0)
	{
		xw = backend->threads.back();

		if(!(xw->started))
		{
			if(fts_xapian_settings.verbose>0) i_info("FTS Xapian : Closing thread because not started : %s",xw->getSummary().c_str());
			delete(xw);
			backend->threads.pop_back();
		}
		else if(xw->terminated)
		{
			if(fts_xapian_settings.verbose>0) i_info("FTS Xapian : Closing thread because terminated : %s",xw->getSummary().c_str());
			delete(xw);
			backend->threads.pop_back();
		}
		else
		{
			n++;
			if((n>50) && (fts_xapian_settings.verbose>0)) 
			{
				for(auto & xwr : backend->threads)
				{
					if((xwr!=NULL)&&(!(xwr->terminated))) i_info("FTS Xapian : Waiting (Sleep4) for thread %s",xwr->getSummary().c_str());
				}
				n=0;
			}
			std::this_thread::sleep_for(XAPIAN_SLEEP);
		}
	}
	if(fts_xapian_settings.verbose>0) i_info("FTS Xapian : All DWs (%s) closed",reason);

	if(backend->dbw!=NULL)
	{
		fts_backend_xapian_close_db(backend->dbw,backend->xap_db,backend->boxname,fts_xapian_settings.verbose);
		backend->dbw=NULL;
	}
}

static bool fts_backend_xapian_isnormalprocess()
{
    	char buff[PATH_MAX];
    	buff[0]=0;
    	ssize_t len = ::readlink("/proc/self/exe", buff, sizeof(buff)-1);
    	if (len>0)  buff[len] = 0;
    	return (strstr(buff,"doveadm")==NULL);
}

XResultSet * fts_backend_xapian_query(Xapian::Database * dbx, XQuerySet * query, long limit=0)
{
	if(fts_xapian_settings.verbose>0) i_info("FTS Xapian: fts_backend_xapian_query (%s)",query->get_string().c_str());

	XResultSet * set= new XResultSet();
	Xapian::Query * q = query->get_query(dbx);

	try
	{
		Xapian::Enquire enquire(*dbx);
		enquire.set_query(*q);
		enquire.set_docid_order(Xapian::Enquire::DESCENDING);

		long offset=0;
		long pagesize=100; if(limit>0) { pagesize=std::min(pagesize,limit); }
		Xapian::MSet m = enquire.get_mset(0, pagesize);
		while(m.size()>0)
		{
			Xapian::MSetIterator i = m.begin();
			while (i != m.end())
			{
				Xapian::Document doc = i.get_document();
				set->add(doc.get_docid());
				i++;
			}
			offset+=pagesize;
			m = enquire.get_mset(offset, pagesize);
		}
	}
	catch(Xapian::Error e)
	{
		i_error("FTS Xapian: xapian_query %s - %s %s",e.get_type(),e.get_msg().c_str(),e.get_error_string());
	}
	delete(q);
	return set;
}

static int fts_backend_xapian_unset_box(struct xapian_fts_backend *backend)
{
	if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: Unset box '%s' (%s)",backend->boxname,backend->guid);

	long commit_time = fts_backend_xapian_current_time();

	fts_backend_xapian_close(backend,"unset box");
	fts_backend_xapian_oldbox(backend);

	if(backend->xap_db != NULL)
	{
		i_free(backend->xap_db);
		backend->xap_db = NULL;

		i_free(backend->guid);
		backend->guid = NULL;

		i_free(backend->boxname);
		backend->boxname = NULL;

		i_free(backend->exp_db);
		backend->exp_db = NULL;

		i_free(backend->dict_db);
                backend->exp_db = NULL;
	}

	return 0;
}

static int fts_backend_xapian_set_path(struct xapian_fts_backend *backend)
{
	struct mail_namespace * ns = backend->backend.ns;
	if(ns->alias_for != NULL)
	{
		if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: Switching namespace");
		ns = ns->alias_for;
	}

	const char * path = mailbox_list_get_root_forced(ns->list, MAILBOX_LIST_PATH_TYPE_INDEX);

	if(backend->path != NULL) i_free(backend->path);
	backend->path = i_strconcat(path, "/" XAPIAN_FILE_PREFIX, static_cast<const char*>(NULL));

	if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: Index path = %s",backend->path);

	struct stat sb;
	if(!( (stat(backend->path, &sb)==0) && S_ISDIR(sb.st_mode)))
	{
		if (mailbox_list_mkdir_root(backend->backend.ns->list, backend->path, MAILBOX_LIST_PATH_TYPE_INDEX) < 0)
		{
			i_error("FTS Xapian: can not create '%s'",backend->path);
			i_error("FTS Xapian: You need to set mail_uid and mail_gid in your dovecot.conf according to the user of mail_location (%s)", path);
			return -1;
		}
	}
	return 0;
}

static int fts_backend_xapian_set_box(struct xapian_fts_backend *backend, struct mailbox *box)
{
	if (box == NULL)
	{
		if(backend->guid != NULL) fts_backend_xapian_unset_box(backend);
		if(fts_xapian_settings.verbose>0) i_warning("FTS Xapian: Box is empty");
		return 0;
	}

	const char * mb;
	fts_mailbox_get_guid(box, &mb );

	if(fts_xapian_settings.verbose>0) i_info("FTS Xapian: Set box '%s' (%s)",box->name,mb);

	if((mb == NULL) || (strlen(mb)<3))
	{
		i_error("FTS Xapian: Invalid box");
		return -1;
	}

	if((backend->guid != NULL) && (strcmp(mb,backend->guid)==0))
	{
		if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: Box is unchanged");
		return 0;
	}

	if(backend->guid != NULL) fts_backend_xapian_unset_box(backend);

	if(fts_backend_xapian_set_path(backend)<0) return -1;

	long current_time = fts_backend_xapian_current_time();

	backend->start_time = current_time;
	backend->lastuid = -1;
	backend->guid = i_strdup(mb);
	backend->boxname = i_strdup(box->name);
	backend->xap_db = i_strdup_printf("%s/db_%s",backend->path,mb);
	backend->exp_db = i_strdup_printf("%s%s",backend->xap_db,suffixExp);
	backend->dict_db = i_strdup_printf("%s%s",backend->xap_db,suffixDict);

	struct stat sb;
	// Verify existence of Dict db
	{
		if(!( (stat(backend->dict_db, &sb)==0) && S_ISREG(sb.st_mode)))
		{
			i_warning("FTS Xapian: '%s' (%s) dictionnary does not exist. Creating it",backend->boxname,backend->dict_db);
			sqlite3 * db = NULL;
                	if(sqlite3_open_v2(backend->dict_db,&db,SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,NULL) != SQLITE_OK )
                	{
                        	i_error("FTS Xapian: Can not open %s : %s",backend->dict_db,sqlite3_errmsg(db));
                	}
                	else
			{	char *zErrMsg = 0;
                		if(sqlite3_exec(db,createDictTable,NULL,0,&zErrMsg) != SQLITE_OK )
                		{
                        		i_error("FTS Xapian: Can not execute (%s) : %s",createDictTable,zErrMsg);
                        		sqlite3_free(zErrMsg);
				}
                        	sqlite3_close(db);
			}
			// Deleting existing indexes
			try
			{
				std::filesystem::remove_all(backend->xap_db);
				std::filesystem::remove(backend->exp_db);
			}
			catch(std::exception e)
			{
				i_error("FTS Xapian: Can not delete old files %s",e.what());
			}
		}
	}
        // Verify existence of Exp db
        {
                if(!( (stat(backend->exp_db, &sb)==0) && S_ISREG(sb.st_mode)))
		{
			i_warning("FTS Xapian: '%s' (%s) expunge does not exist. Creating it",backend->boxname,backend->exp_db);
                        sqlite3 * db = NULL;
                        if(sqlite3_open_v2(backend->exp_db,&db,SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,NULL) != SQLITE_OK )
                        {
                                i_error("FTS Xapian: Can not open %s : %s",backend->exp_db,sqlite3_errmsg(db));
                        }
                        else
                        {
                                char *zErrMsg = 0;
                                if(sqlite3_exec(db,createExpTable,NULL,0,&zErrMsg) != SQLITE_OK )
                                {
                                        i_error("FTS Xapian: Can not execute (%s) : %s",createExpTable,zErrMsg);
                                        sqlite3_free(zErrMsg);
                                }
                                sqlite3_close(db);
                        }
                }
        }
	// Verify existence of Xapian db
	{
		char * t = i_strdup_printf("%s/termlist.glass",backend->xap_db);
        	if(!( (stat(t, &sb)==0) && S_ISREG(sb.st_mode)))
        	{
                	i_info("FTS Xapian: '%s' (%s) indexes do not exist. Initializing DB",backend->boxname,backend->xap_db);
                	try
                	{
                	        Xapian::WritableDatabase * db = new Xapian::WritableDatabase(backend->xap_db,Xapian::DB_CREATE_OR_OVERWRITE | Xapian::DB_BACKEND_GLASS);
                	        db->close();
                	        delete(db);
                	}
                	catch(Xapian::Error e)
                	{
                	        i_error("FTS Xapian: Can't create Xapian DB (%s) %s : %s - %s %s",backend->boxname,backend->xap_db,e.get_type(),e.get_msg().c_str(),e.get_error_string());
                	}
        	}
        	i_free(t);
	}
        backend->threads.clear();
	backend->total_docs =0;

	return 0;
}

static void fts_backend_xapian_build_qs(XQuerySet * qs, struct mail_search_arg *a, const char * dict=NULL)
{
	const char * hdr;

	if(fts_xapian_settings.verbose>1) i_info("FTS Xapian: fts_backend_xapian_build_qs");

	while(a != NULL)
	{
		switch (a->type)
		{
			case SEARCH_TEXT:
			case SEARCH_BODY:
			case SEARCH_HEADER:
			case SEARCH_HEADER_ADDRESS:
			case SEARCH_HEADER_COMPRESS_LWSP: break;
			default: a = a->next; continue;
		}

		if((a->hdr_field_name == NULL)||(strlen(a->hdr_field_name)<1))
		{
			if(a->type == SEARCH_BODY)
			{
				hdr="body";
			}
			else
			{
				hdr=XAPIAN_WILDCARD;
			}
		}
		else
		{
			hdr=a->hdr_field_name;
		}
		if((a->value.str == NULL) || (strlen(a->value.str)<1))
		{
			XQuerySet * q2;
			if(a->match_not)
			{
				q2 = new XQuerySet(Xapian::Query::OP_AND_NOT,qs->limit);
			}
			else
			{
				q2 = new XQuerySet(Xapian::Query::OP_OR,qs->limit);
			}
			fts_backend_xapian_build_qs(q2,a->value.subargs,dict);
			if(q2->count()>0)
			{
				qs->add(q2);
			}
			else
			{
				delete(q2);
			}
		}
		else if(dict != NULL)
		{
			//Clean header
			std::string header; header.clear();
			long i=0,j=strlen(hdr);
                        while(i<j)
                        {
                        	if((hdr[i]>' ') && (hdr[i]!='"') && (hdr[i]!='\'') && (hdr[i]!='-'))
                                {
                                        header+=tolower(hdr[i]);
                                }
                                i++;
			}

			// Find key words
			icu::StringPiece sp(a->value.str);
                        icu::UnicodeString t = icu::UnicodeString::fromUTF8(sp);
                        fts_backend_xapian_clean(&t);
			i = t.lastIndexOf(CHAR_SPACE),j;
			icu::UnicodeString *k;
			std::vector<icu::UnicodeString *> keys; keys.clear();
			while(i>0)
			{
				j = t.length();
                                k = new icu::UnicodeString(t,i+1,j-i-1);
				if(k->length()>1) { keys.push_back(k); } else delete(k);
				t.truncate(i);
                               	fts_backend_xapian_trim(&t);
                               	i = t.lastIndexOf(CHAR_SPACE);
                        }
			if(t.length()>1) 
			{
				keys.push_back(new icu::UnicodeString (t));
			}

			// For each key, search dictionnary
                        sqlite3 * db = NULL;
			char * zErrMsg =0;
                        if(sqlite3_open_v2(dict,&db,SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_READONLY,NULL) != SQLITE_OK )
                        {
                                syslog(LOG_ERR,"FTS Xapian: Can not open %s : %s",dict,sqlite3_errmsg(db));
                                return;
                        }
			// Generate query
			XQuerySet * q1, *q2;
                        if(a->match_not)
                        {
                                q1 = new XQuerySet(Xapian::Query::OP_AND_NOT,qs->limit);
                        }
                        else
                        {
                                q1 = new XQuerySet(Xapian::Query::OP_AND,qs->limit);
                        }	
			for(auto & ki : keys)
			{
				std::vector<icu::UnicodeString *> st; st.clear();
				std::string sql=searchDict1;
				ki->toUTF8String(sql);
				sql +=searchDict2;
				if(sqlite3_exec(db,sql.c_str(),fts_backend_xapian_sqlite3_vector_icu,&st,&zErrMsg) != SQLITE_OK )
                                {
                                        syslog(LOG_ERR,"FTS Xapian: Can not search keyword : %s",sql.c_str(),zErrMsg);
                                        sqlite3_free(zErrMsg);
                                }
				q2 = new XQuerySet(Xapian::Query::OP_OR,qs->limit);
                                for(auto &term : st)
                                {
                                        q2->add(header.c_str(),term,false);
                                        delete(term);
                                }
                                q1->add(q2);
				delete(ki);
			}
			qs->add(q1);
			sqlite3_close(db);
		}
		else
		{
			long i=0,j=strlen(hdr);
			std::string f2; f2.clear();
			while(i<j)
			{
				if((hdr[i]>' ') && (hdr[i]!='"') && (hdr[i]!='\'') && (hdr[i]!='-'))
				{
					f2+=tolower(hdr[i]);
				}
				i++;
			}
			icu::StringPiece sp(a->value.str);
			icu::UnicodeString t = icu::UnicodeString::fromUTF8(sp);
			fts_backend_xapian_clean(&t);
	
			char * h = i_strdup(f2.c_str());
			qs->add(h,&t,a->match_not);
			i_free(h);
		}
		a->match_always=true;
		a = a->next;
	}
}

