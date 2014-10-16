#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "cachesim.hpp"

int Tag_Size;// = 64-(DEFAULT_C-DEFAULT_S);
int Lines_Per_Set;// = power(2,DEFAULT_S);
int Index_Size;// = DEFAULT_C-DEFAULT_B-DEFAULT_S;
int Num_Sets;// = power(2,DEFAULT_C-DEFAULT_B-DEFAULT_S);
int Num_V_Blocks;// = DEFAULT_V;
int Prefetch_Length;// = DEFAULT_K;
int Block_Size;// = power(2,DEFAULT_B);
int S_number;
uint64_t upperbound = 0xfffffffffffe0000;//for prefetch address check (not useful in these experiments)
bool negative_stride = false;
bool inherit_dirty;
int inherit_choice;
bool inherit_dirty_vc;
int inherit_choice_vc;
uint64_t Last_Miss_Block_Address = 0;
uint64_t pending_stride = 0; // pending stride
class cacheline;//block
class set;//set
class cache;//cache
class driver;//set up everything and perform the process

int getIndex(char* addrBin);//get index from a given address
char* DecToBin(uint64_t address);//turn the address from uint64_t(decimal) into char(binary)
uint64_t Block_Address(uint64_t address);//get the block address for prefetch
int power(int a, uint64_t n);//realize the function pow() in <cmath>
bool tagCompare(char* a, char* b, int N);//compare the given address

void print_statistics(cache_stats_t* p_stats);//print statistics


void print_help_and_exit(void) {
    printf("cachesim [OPTIONS] < traces/file.trace\n");
    printf("  -c C\t\tTotal size in bytes is 2^C\n");
    printf("  -b B\t\tSize of each block in bytes is 2^B\n");
    printf("  -s S\t\tNumber of blocks per set is 2^S\n");
    printf("  -v V\t\tNumber of blocks in victim cache\n");
    printf("  -k K\t\tPrefetch Distance");
	printf("  -h\t\tThis helpful output\n");
    exit(0);
}



// just as pow() function in <cmath>
int power(int a, uint64_t n){
    int ans = 1;
    for(uint64_t i=0; i<n; i++){
        ans = ans * a;
    }
    return ans;
}

// compare the tow given address
bool tagCompare(char* a, char* b, int N){
    int count=0;
    for(int i=0;i<N;++i){
        if(a[i]==b[i])count++;
    }
    return count==N;
}

//return Index
int getIndex(char* addrBin){
    int index=0;
    for(int i=0;i<Index_Size;i++){
        if(addrBin[Tag_Size+i] == '1')
        index += power(2, Index_Size-1-i);
    }
        return index;
}


// turn the address into binary form stored in char
char* DecToBin(uint64_t address){
    char* addrBin = new char[65];
    for(uint64_t i=0;i<64;++i){
        if( (address & (uint64_t(1)<<i))>>i) addrBin[63-i]='1';
        else addrBin[63-i]='0';
    }
    addrBin[64]='\0';
    return addrBin;
}

// get block address for prefetch
uint64_t Block_Address(uint64_t address){
    uint64_t o = 0xffffffffffffffe0;
    return (address & o);
}

/* this class simulates data structure of a single block */
class cacheline{
    
public:
    char tag[65];//as long as the address length, simpilify the different length for different scenarios
    bool valid;//valid bits
    bool dirty;//dirty bits
    bool prefetch;//prefetch bits
    int timestamp; //age
    
    
    cacheline(){//constructor
        for(int i=0;i<64;++i) tag[i]='x';//initialize tag to random chars
        tag[64]='\0';
        valid=false;
        dirty=false;
        prefetch=false;
        timestamp=0;
    }
    
};



/* this class contains the data structure to simulate a set,
 *LRU/FIFO policy and eviction algorithm are implemented here */
class set{
public:
    
    cacheline* cachelines[2048];
    
    set(){//constructor
        //use the bigger one of Lines_Per_Set and Num_V_Blocks, do not differentiate the victim cache with L1 cache
        for(int i=0;i < (Lines_Per_Set>Num_V_Blocks ? Lines_Per_Set:Num_V_Blocks);++i){
            cachelines[i]=new cacheline();
        }
    }
    
    //this function updates LRU bits of lines, given the hitted line index
    void updateLRU(int hit){
        for(int i=0;i<(Lines_Per_Set>Num_V_Blocks ? Lines_Per_Set:Num_V_Blocks);++i){
            if(cachelines[i]->valid){
                cachelines[i]->timestamp++;
            }
        }
        cachelines[hit]->timestamp=0;
    }
    //exactly the same with LRU, just to make clear of what's going on...really hard to debug
    void updateFIFO(int hit){
        for(int i=0;i<(Lines_Per_Set>Num_V_Blocks ? Lines_Per_Set:Num_V_Blocks);++i){
            if(cachelines[i]->valid)
                cachelines[i]->timestamp++;
        }
        cachelines[hit]->timestamp=0;
    }
    
    //this function moves a new block into the set when empty block available in a paticular set
    void moveBlockIn_LRU(char* addrBin, cache_stats_t* p_stats){
        int choice=0;
        for(int i=0;i<Lines_Per_Set;i++){
            if(!cachelines[i]->valid){
                choice = i;
                break;
            }
        }
        inherit_choice = choice;
        for(int j=0;j<64;++j) cachelines[choice]->tag[j]=addrBin[j];
        cachelines[choice]->valid = true;
        cachelines[choice]->dirty = false;
        cachelines[choice]->prefetch = false;
        updateLRU(choice);
        p_stats->bytes_transferred += Block_Size;
    }
    
    
    //for write process to set drity bit
    void setDirty(char* addrBin){
        for(int i=0;i<Lines_Per_Set;i++){
            if(tagCompare(cachelines[i]->tag, addrBin, Tag_Size+Index_Size)){
                cachelines[i]->dirty = true;
            }
        }
    }
    
    //for prefetch to set prefetch bits
    void setPrefetch(char* addrBin){
        for(int i=0;i<Lines_Per_Set;i++){
            if(tagCompare(cachelines[i]->tag, addrBin, Tag_Size+Index_Size)){
                cachelines[i]->prefetch = true;
            }
        }
    }
    
};

/* this calss simulates a cache, including L1 and VC in one class! */
class cache{
public:
    
    set* sets[2049];
    
    /* little trick here, use set up one more set for cache, use the additional one as VC!
     * use sets[Num_Sets] to function as VC */
    cache(){
        for(int i=0;i<(Num_Sets+1);i++){
            sets[i] = new set();
        }
    }
    
    //test whether it's a hit or not on L1
    bool isHit_cache(char* addrBin, int setIndex){
        for(int i=0;i<Lines_Per_Set;i++){
            if(sets[setIndex]->cachelines[i]->valid){
                //supposed to use just Tag_Size here, longer compare to make me safe from changing length
                if(tagCompare(sets[setIndex]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                    return true;
            }
        }
        return false;
    }
    //test whether it's a hit or not on VC
    bool isHit_victim(char* addrBin, int setIndex){
        for(int i=0;i<Num_V_Blocks;i++){
            //have to use Tag_Size+Index_Size, VC is fully associative
            if(tagCompare(sets[Num_Sets]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size)
               && sets[Num_Sets]->cachelines[i]->valid) {
                return true;
            }
        }
        return false;
    }
    
    //check whether there is an empty block or not in L1
    bool isEmptyAvailable(int setIndex){
        for(int i=0; i<Lines_Per_Set;i++){
            if(!sets[setIndex]->cachelines[i]->valid){
                return true;
                
            }
        }
        return false;
    }
    
    //read process
    void read(char* addrBin, uint64_t address, cache_stats_t* p_stats){
        p_stats->accesses++;
        int setIndex = getIndex(addrBin);
        p_stats->reads++;
        //miss in L1
        if(!isHit_cache(addrBin, setIndex)){
            p_stats->read_misses++;
            p_stats->misses++;
            //miss in VC
            if(!isHit_victim(addrBin, setIndex)){
                p_stats->vc_misses++;
                p_stats->read_misses_combined++;
                //if empty block available in the set, just move in
                if(isEmptyAvailable(setIndex)){
                    sets[setIndex]->moveBlockIn_LRU(addrBin, p_stats);
                }
                else{
                    //special case for no VC
                    if(Num_V_Blocks==0){
                        moveInCache_V0(addrBin, setIndex, p_stats);
                    }
                    //with VC, evict block to victim and bringing a new one
                    else{
                        moveBlocktoVictim(addrBin,setIndex,p_stats);
                    }
                }
            }
            //hit in VC
            else{
                //swap hit block in VC with LRU block in L1
                replaceWithVictim(addrBin, setIndex, p_stats);
                //check whether it's a prefetch or not
                if(sets[setIndex]->cachelines[inherit_choice]->prefetch) {
                    p_stats->useful_prefetches++;//if it's a prefetch block, it's useful
                    sets[setIndex]->cachelines[inherit_choice]->prefetch = false;// reset prefetch bit
                }
            }
            //prefetch
            uint64_t d = 0;
            if( negative_stride == true){
                d = Last_Miss_Block_Address - address;
            }
            else{
                d = address - Last_Miss_Block_Address;
            }
            if( d == pending_stride){
                for(uint64_t i=1;i<=Prefetch_Length;i++){
                    //in case of prefetch out of the 64-bit address, does not matter due to the inputs by trace
                    if(!negative_stride)
                        if(upperbound-address < i*pending_stride){
                            break;
                        }
                    if(negative_stride)
                        if(address- 0 < i*pending_stride){
                            break;
                        }
                    p_stats->prefetched_blocks++;
                    uint64_t pre_addr;
                    if(!negative_stride){
                        pre_addr = address + i * pending_stride;
                    }
                    else{
                        pre_addr = address - i * pending_stride;
                    }
                    char* pre_addrBin = new char[65];
                    pre_addrBin = DecToBin(pre_addr);
                    int pre_setIndex;
                    pre_setIndex = getIndex(pre_addrBin);
                    if(isHit_cache(pre_addrBin, pre_setIndex)){
                        p_stats->bytes_transferred += Block_Size;//dumb prefetch
                    }
                    //prefetch hit in VC, swap & set prefetch bits
                    else if(isHit_victim(pre_addrBin, pre_setIndex)){
                        replaceWithVictim(pre_addrBin, pre_setIndex, p_stats);
                        p_stats->bytes_transferred += Block_Size;
                        sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        //set prefetched block's timestamp as the biggest(LRU)
                        int tmp=0;
                        for(int j=0;j<Lines_Per_Set;j++){
                            if(sets[pre_setIndex]->cachelines[j]->timestamp >= tmp){
                                tmp = sets[pre_setIndex]->cachelines[j]->timestamp;
                            }
                        }
                        sets[pre_setIndex]->cachelines[inherit_choice]->timestamp = tmp + 1;
                    }
                    //prefetch miss in both L1 and VC
                    else if(isEmptyAvailable(pre_setIndex)){
                        //empty blocks available in L1 set, bring in & set prefetch bits
                        sets[pre_setIndex]->moveBlockIn_LRU(pre_addrBin,p_stats);
                        sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        //set prefetched block's timestamp as the biggest(LRU)
                        int tmp=0;
                        for(int j=0;j<Lines_Per_Set;j++){
                            if(sets[pre_setIndex]->cachelines[j]->timestamp >= tmp){
                                tmp = sets[pre_setIndex]->cachelines[j]->timestamp;
                            }
                        }
                        sets[pre_setIndex]->cachelines[inherit_choice]->timestamp = tmp + 1;
                    }
                    else {
                        // special for V=0
                        if(Num_V_Blocks==0){
                            moveInCache_V0(pre_addrBin, pre_setIndex, p_stats);
                            sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        }
                        // evict LRU block to VC, prefetch into its position in LRU
                        else{
                            moveBlocktoVictim(pre_addrBin, pre_setIndex,p_stats);
                            sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        }
                        //set prefetched block's timestamp as the biggest(LRU)
                        int tmp=0;
                        for(int j=0;j<Lines_Per_Set;j++){
                            if(sets[pre_setIndex]->cachelines[j]->timestamp >= tmp){
                                tmp = sets[pre_setIndex]->cachelines[j]->timestamp;
                            }
                        }
                        sets[pre_setIndex]->cachelines[inherit_choice]->timestamp = tmp + 1;
                    }
                }
            }
            //does not match pending_stride, do not prefetch, set new pending stride
            else{
                if(address<Last_Miss_Block_Address){
                    negative_stride = true;
                    pending_stride = Last_Miss_Block_Address - address;
                }
                else{
                    negative_stride = false;
                    pending_stride = address - Last_Miss_Block_Address;
                }
            }

            Last_Miss_Block_Address = address;
        }
        //hit in L1
        else{
            //update hit block's timestamp
            for(int i=0;i<Lines_Per_Set;i++)
                if(sets[setIndex]->cachelines[i]->valid)
                    if(tagCompare(sets[setIndex]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                        sets[setIndex]->updateLRU(i);//hit 后更改timestamp
            //just check if hitted block is prefetched
            for(int i=0;i<Lines_Per_Set;i++){
                if(tagCompare(sets[setIndex]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                    if(sets[setIndex]->cachelines[i]->prefetch) {
                        p_stats->useful_prefetches++;//if it's a prefetch block, it's useful
                        sets[setIndex]->cachelines[i]->prefetch = false;// reset prefetch bit
                    }
            }
        }
    }
    
    //write process
    void write(char* addrBin, uint64_t address, cache_stats_t* p_stats){
        p_stats->accesses++;
        int setIndex = getIndex(addrBin);
        p_stats->writes++;
        //hit in L1
        if(isHit_cache(addrBin, setIndex)){
            //update hit block's timestamp
            for(int i=0;i<Lines_Per_Set;i++)
                if(sets[setIndex]->cachelines[i]->valid)
                    if(tagCompare(sets[setIndex]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                        sets[setIndex]->updateLRU(i);
            sets[setIndex]->setDirty(addrBin); //set Dirty for write!!!!
            //check whether it is a prefetched block or not
            for(int i=0;i<Lines_Per_Set;i++){
                if(tagCompare(sets[setIndex]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                    if(sets[setIndex]->cachelines[i]->prefetch) {
                        p_stats->useful_prefetches++;//if it's a prefetch block, it's useful
                        sets[setIndex]->cachelines[i]->prefetch = false;// reset prefetch bit
                    }
            }
        }
        //miss in L1
        else{
            p_stats->write_misses++;
            p_stats->misses++;
            //miss in VC
            if(!isHit_victim(addrBin,setIndex)){
                p_stats->vc_misses++;
                p_stats->write_misses_combined++;
                //empty blocks available in L1
                if(isEmptyAvailable(setIndex)){
                    sets[setIndex]->moveBlockIn_LRU(addrBin,p_stats);
                    sets[setIndex]->setDirty(addrBin);//set Dirty!!
                }
                //L1 is full, evict LRU and bring new in
                else{
                    if(Num_V_Blocks==0){
                        moveInCache_V0(addrBin, setIndex, p_stats);
                        sets[setIndex]->setDirty(addrBin);
                    }
                    else{
                        moveBlocktoVictim(addrBin,setIndex,p_stats);
                        sets[setIndex]->setDirty(addrBin);
                    }
                }
            }
            //hit in VC
            else{
                //swap & set prefetch
                replaceWithVictim(addrBin, setIndex, p_stats);
                sets[setIndex]->setDirty(addrBin);//set dirty for writes
                for(int i=0;i<Lines_Per_Set;i++){
                    if(tagCompare(sets[setIndex]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                        if(sets[setIndex]->cachelines[i]->prefetch) {
                            p_stats->useful_prefetches++;//if it's a prefetch block, it's useful
                            sets[setIndex]->cachelines[i]->prefetch = false;// reset prefetch bit
                        }
                }
            }
            
            uint64_t d = 0;
            //prefetch
            if(negative_stride == true){
                d = Last_Miss_Block_Address - address;
            }
            else{
                d = address - Last_Miss_Block_Address;
            }
            if( d == pending_stride){
                for(uint64_t i=1;i<=Prefetch_Length;i++){
                    //make sure do not prefetch sth out of 64-bit address, useless in these experiments
                    if(!negative_stride)
                        if(upperbound-address < i*pending_stride){
                            break;
                        }
                    if(negative_stride)
                        if(address-0 < i*pending_stride){
                            break;
                        }
                    p_stats->prefetched_blocks++;
                    uint64_t pre_addr;
                    if(!negative_stride){
                        pre_addr = address + i * pending_stride;
                    }
                    else{
                        pre_addr = address - i * pending_stride;
                    }
                    char* pre_addrBin = new char[65];
                    pre_addrBin = DecToBin(pre_addr);
                    int pre_setIndex;
                    pre_setIndex = getIndex(pre_addrBin);
                    if(isHit_cache(pre_addrBin, pre_setIndex)){
                        p_stats->bytes_transferred += Block_Size;
                    }
                    else if(isHit_victim(pre_addrBin, pre_setIndex)){
                        replaceWithVictim(pre_addrBin, pre_setIndex, p_stats);
                        p_stats->bytes_transferred += Block_Size;
                        sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        //set prefetched block's timestamp as the biggest(LRU)
                        int tmp=0;
                        for(int j=0;j<Lines_Per_Set;j++){
                            if(sets[pre_setIndex]->cachelines[j]->timestamp >= tmp){
                                tmp = sets[pre_setIndex]->cachelines[j]->timestamp;
                            }
                        }
                        sets[pre_setIndex]->cachelines[inherit_choice]->timestamp = tmp + 1;
                    }
                    else if(isEmptyAvailable(pre_setIndex)){
                        
                        //p_stats->prefetched_blocks++;
                        sets[pre_setIndex]->moveBlockIn_LRU(pre_addrBin,p_stats);
                        sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        //set prefetched block's timestamp as the biggest(LRU)
                        int tmp=0;
                        for(int j=0;j<Lines_Per_Set;j++){
                            if(sets[pre_setIndex]->cachelines[j]->timestamp >= tmp){
                                tmp = sets[pre_setIndex]->cachelines[j]->timestamp;
                            }
                        }
                        sets[pre_setIndex]->cachelines[inherit_choice]->timestamp = tmp + 1;
                    }
                    else {
                        if(Num_V_Blocks==0){
                            moveInCache_V0(pre_addrBin, pre_setIndex, p_stats);
                            sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        }
                        else{
                            moveBlocktoVictim(pre_addrBin, pre_setIndex,p_stats);//VICTIM
                            sets[pre_setIndex]->setPrefetch(pre_addrBin);
                        }
                        
                        int tmp=0;
                        for(int j=0;j<Lines_Per_Set;j++){
                            if(sets[pre_setIndex]->cachelines[j]->timestamp >= tmp){
                                tmp = sets[pre_setIndex]->cachelines[j]->timestamp;
                            }
                        }
                        sets[pre_setIndex]->cachelines[inherit_choice]->timestamp = tmp + 1;
                    }
                }
            }
            //does not match pending_stride
            else{
                if(address<Last_Miss_Block_Address){
                    negative_stride = true;
                    pending_stride = Last_Miss_Block_Address - address;
                }
                else{
                    negative_stride = false;
                    pending_stride = address - Last_Miss_Block_Address;
                }
            }
    
            Last_Miss_Block_Address = address;
        }
    }
    
    //evict function when v=0
    void moveInCache_V0(char* addrBin, int setIndex, cache_stats_t* p_stats){
        //choose the LRU blocks in L1
        int choice=0;
        int tmp=0;
        for(int i=0;i<Lines_Per_Set;i++){
            if(sets[setIndex]->cachelines[i]->timestamp >= tmp){
                choice = i;
                tmp = sets[setIndex]->cachelines[i]->timestamp;
            }
        }
        //if dirty, write back
        if(sets[setIndex]->cachelines[choice]->dirty){
            p_stats->bytes_transferred += Block_Size;
            p_stats->write_backs++;
        }
        //bring in new block from main mem
        for(int i=0;i<(64);i++){
            sets[setIndex]->cachelines[choice]->tag[i] = addrBin[i];
        }
        sets[setIndex]->cachelines[choice]->dirty = false;
        sets[setIndex]->cachelines[choice]->prefetch = false;
        sets[setIndex]->cachelines[choice]->valid = true;
        sets[setIndex]->updateLRU(choice);
        p_stats->bytes_transferred += Block_Size;
        inherit_choice=choice;
        
    }
    
    //swap function for VC hit
    void replaceWithVictim(char* addrBin, int setIndex, cache_stats_t* p_stats){
        //choose LRU in L1
        int choice=0;
        int tmp=0;
        for(int i=0;i<Lines_Per_Set;i++){
            if(sets[setIndex]->cachelines[i]->timestamp >= tmp){
                choice = i;
                tmp = sets[setIndex]->cachelines[i]->timestamp;
            }
        }
        inherit_choice = choice;
        //find the hit block in VC
        int choice_vc;
        for(int i=0;i<Num_V_Blocks;i++){
            if(tagCompare(sets[Num_Sets]->cachelines[i]->tag, addrBin, Tag_Size+Index_Size))
                choice_vc=i;
        }
        inherit_choice_vc = choice_vc;
        //swap block between L1 and VC, no need to do real swap
        for(int j=0;j<(64);++j)
            sets[Num_Sets]->cachelines[choice_vc]->tag[j]=sets[setIndex]->cachelines[choice]->tag[j];
        for(int i=0;i<(64);i++){
            sets[setIndex]->cachelines[choice]->tag[i] = addrBin[i];
        }
        //swap dirty, prefetch status
        bool tmp_dirty;
        bool tmp_prefetch;
        
        tmp_dirty = sets[Num_Sets]->cachelines[choice_vc]->dirty;
        sets[Num_Sets]->cachelines[choice_vc]->dirty = sets[setIndex]->cachelines[choice]->dirty;
        sets[setIndex]->cachelines[choice]->dirty = tmp_dirty;
        
        tmp_prefetch = sets[Num_Sets]->cachelines[choice_vc]->prefetch;
        sets[Num_Sets]->cachelines[choice_vc]->prefetch = sets[setIndex]->cachelines[choice]->prefetch;
        sets[setIndex]->cachelines[choice]->prefetch = tmp_prefetch;
        //update timestamp in L1
        sets[setIndex]->updateLRU(choice);
        
        
    }
    
    //evict function
    void moveBlocktoVictim(char* addrBin, int setIndex, cache_stats_t* p_stats){
        //choose LRU in L!
        int choice=0;
        int tmp=0;
        for(int i=0;i<Lines_Per_Set;i++){
            if(sets[setIndex]->cachelines[i]->timestamp >= tmp){
                choice = i;
                tmp = sets[setIndex]->cachelines[i]->timestamp;
            }
        }
        //choose a VC block
        int choice_vc;
        //if VC is not full, put the evicted one into the empty position
        bool isEmpty=false;
        for(int i=0;i<Num_V_Blocks;i++){
            if(!sets[Num_Sets]->cachelines[i]->valid){
                choice_vc=i;
                isEmpty=true;
                break;
            }
        }
        if(isEmpty){
            for(int j=0;j<(64);++j)
                sets[Num_Sets]->cachelines[choice_vc]->tag[j]=sets[setIndex]->cachelines[choice]->tag[j];
            sets[Num_Sets]->cachelines[choice_vc]->valid=true;
            sets[Num_Sets]->cachelines[choice_vc]->dirty=sets[setIndex]->cachelines[choice]->dirty;
            sets[Num_Sets]->cachelines[choice_vc]->prefetch=sets[setIndex]->cachelines[choice]->prefetch;
            //            sets[Num_Sets]->cachelines[choice_vc]->prefetch=false;
            sets[Num_Sets]->updateFIFO(choice_vc);
        }
        //VC is full
        else{
            //find the oldest one VC
            choice_vc=0;
            int tmp=0;
            for(int i=0;i<Num_V_Blocks;i++){
                if(sets[Num_Sets]->cachelines[i]->timestamp >= tmp){
                    choice_vc = i;
                    tmp = sets[Num_Sets]->cachelines[i]->timestamp;
                }
            }
            //dirty, write back
            if(sets[Num_Sets]->cachelines[choice_vc]->dirty){
                p_stats->bytes_transferred += Block_Size;
                p_stats->write_backs++;
            }
            
            for(int j=0;j<(64);++j)
                sets[Num_Sets]->cachelines[choice_vc]->tag[j]=sets[setIndex]->cachelines[choice]->tag[j];
            sets[Num_Sets]->cachelines[choice_vc]->dirty=sets[setIndex]->cachelines[choice]->dirty;
            sets[Num_Sets]->cachelines[choice_vc]->prefetch=sets[setIndex]->cachelines[choice]->prefetch;
            sets[Num_Sets]->updateFIFO(choice_vc);
        }
        //set up new block's tag on cache
        for(int i=0;i<(64);i++){
            sets[setIndex]->cachelines[choice]->tag[i] = addrBin[i];
        }
        sets[setIndex]->cachelines[choice]->dirty = false;
        sets[setIndex]->cachelines[choice]->prefetch = false;
        sets[setIndex]->cachelines[choice]->valid = true;
        sets[setIndex]->updateLRU(choice);
        inherit_choice=choice;
        p_stats->bytes_transferred += Block_Size;
    }
    
    
};


//set up the cache
class driver{
public:
    cache* mycache = new cache();
    
    driver(){};
    
    void cache_access(char rw, uint64_t address, cache_stats_t* p_stats) {
        if(rw=='r'){
            mycache->read(DecToBin(address), Block_Address(address), p_stats);
        }
        else{
            mycache->write(DecToBin(address),Block_Address(address), p_stats);
        }
    }
    
    // calculating stats
    void complete_cache(cache_stats_t *p_stats) {
        p_stats->hit_time = 2 + 0.2 * S_number;
        p_stats->miss_penalty = 200;
        p_stats->miss_rate = ((double)p_stats->misses )/ ((double)p_stats->accesses);
        p_stats->avg_access_time = (double)p_stats->hit_time + ((double)p_stats->vc_misses/(double)p_stats->accesses) * ((double)p_stats->miss_penalty);
    }
};




int main(int argc, char* argv[]) {
    int opt;
    uint64_t c = DEFAULT_C;
    uint64_t b = DEFAULT_B;
    uint64_t s = DEFAULT_S;
    uint64_t v = DEFAULT_V;
	uint64_t k = DEFAULT_K;
    FILE* fin  = stdin;

    /* Read arguments */
    while(-1 != (opt = getopt(argc, argv, "c:b:s:i:v:k:h"))) {
        switch(opt) {
        case 'c':
            c = atoi(optarg);
            break;
        case 'b':
            b = atoi(optarg);
            break;
        case 's':
            s = atoi(optarg);
            break;
        case 'v':
            v = atoi(optarg);
            break;
		case 'k':
			k = atoi(optarg);
			break;
        case 'i':
            fin = fopen(optarg, "r");
            break;
        case 'h':
            /* Fall through */
        default:
            print_help_and_exit();
            break;
        }
    }
    
    Tag_Size = 64-(c-s);
    Lines_Per_Set = power(2,s);
    Index_Size = c-b-s;
    Num_Sets = power(2,c-b-s);
    Num_V_Blocks = v;
    Prefetch_Length = k;
    Block_Size = power(2,b);
    S_number = s;

    printf("Cache Settings\n");
    printf("C: %" PRIu64 "\n", c);
    printf("B: %" PRIu64 "\n", b);
    printf("S: %" PRIu64 "\n", s);
    printf("V: %" PRIu64 "\n", v);
	printf("K: %" PRIu64 "\n", k);
    printf("\n");

    /* Setup the cache */
    
    
    driver* mydriver = new driver();
    
    /* Setup statistics */
    cache_stats_t stats;
    memset(&stats, 0, sizeof(cache_stats_t));
    
    /* Begin reading the file */
    char rw;
    uint64_t address;
    while (!feof(fin)) {
        int ret = fscanf(fin, "%c %" PRIx64 "\n", &rw, &address);
        if(ret == 2) {
            mydriver->cache_access(rw, address, &stats);
        }
    }

    mydriver->complete_cache(&stats);

    print_statistics(&stats);

    return 0;
}

void print_statistics(cache_stats_t* p_stats) {
    printf("Cache Statistics\n");
    printf("Accesses: %" PRIu64 "\n", p_stats->accesses);
    printf("Reads: %" PRIu64 "\n", p_stats->reads);
    printf("Read misses: %" PRIu64 "\n", p_stats->read_misses);
    printf("Read misses combined: %" PRIu64 "\n", p_stats->read_misses_combined);
    printf("Writes: %" PRIu64 "\n", p_stats->writes);
    printf("Write misses: %" PRIu64 "\n", p_stats->write_misses);
    printf("Write misses combined: %" PRIu64 "\n", p_stats->write_misses_combined);
    printf("Misses: %" PRIu64 "\n", p_stats->misses);
    printf("Writebacks: %" PRIu64 "\n", p_stats->write_backs);
	printf("Victim cache misses: %" PRIu64 "\n", p_stats->vc_misses);
	printf("Prefetched blocks: %" PRIu64 "\n", p_stats->prefetched_blocks);
	printf("Useful prefetches: %" PRIu64 "\n", p_stats->useful_prefetches);
	printf("Bytes transferred to/from memory: %" PRIu64 "\n", p_stats->bytes_transferred);
	printf("Hit Time: %f\n", p_stats->hit_time);
    printf("Miss Penalty: %" PRIu64 "\n", p_stats->miss_penalty);
    printf("Miss rate: %f\n", p_stats->miss_rate);
    printf("Average access time (AAT): %f\n", p_stats->avg_access_time);
}



