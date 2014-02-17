/*
 * BlockStreamSortIterator.cpp
 *
 *  Created on: 2014-2-17
 *      Author: casa
 */

#include "BlockStreamSortIterator.h"

BlockStreamSortIterator::BlockStreamSortIterator(){
    sema_open_.set_value(1);
    sema_open_finished_.set_value(0);
}

BlockStreamSortIterator::BlockStreamSortIterator(State state)
:finished_thread_count_(0),registered_thread_count_(0),state_(state){
    sema_open_.set_value(1);
    sema_open_finished_.set_value(0);
}

BlockStreamSortIterator::~BlockStreamSortIterator(){

}

BlockStreamSortIterator::State::State()
:input_(0),orderbyKey_(0),child_(0),block_size_(0),partition_offset_(0){

}

BlockStreamSortIterator::State::State(Schema* input,unsigned orderbyKey,BlockStreamIteratorBase* child,const unsigned block_size,const PartitionOffset partition_offset)
:input_(input),orderbyKey_(orderbyKey),child_(child),block_size_(block_size),partition_offset_(partition_offset){

}

void BlockStreamSortIterator::swap(void *&desc,void *&src){
	swap_num++;
	void *temp;
	temp=desc;
	desc=src;
	src=temp;
}

void BlockStreamSortIterator::cqsort(int left,int right){
	int front=left+1;
	int end=right;
	//TODO:	orderby key can be any one of the column
	int key_int=*(int *)secondaryArray[left];
	if(left>=right)
		return;
	while(1){
		while(*(int *)secondaryArray[end]>key_int)
			end--;
		while(*(int *)secondaryArray[front]<key_int&&front<end)
			front++;
		if(front>=end)
			break;
		void * &x=secondaryArray[front];
		void * &y=secondaryArray[end];
		swap(x,y);
		if(*(int *)secondaryArray[front]==key_int)
			end--;
		else
			front++;
	}
	void *&m=secondaryArray[left];
	void *&n=secondaryArray[end];
	swap(m,n);
	if(left<front-1)
		cqsort(left,front-1);
	if(end+1<right)
		cqsort(end+1,right);

}

void BlockStreamSortIterator::order(unsigned column,unsigned tuple_count){
	/* tranverse the buffer and apply the space to store the secondaryArray*/
	cqsort(0,tuple_count-1);
}

bool BlockStreamSortIterator::open(const PartitionOffset& part_off){
	//TODO: multi threads can be used to pipeline!!!
	swap_num=0;
	temp_cur=0;
	/* first we can store all the data which will be bufferred
	 * 1, buffer is the first phase. multi-threads will be applyed to the data
	 *    in the buffer.
	 * 2, sort the data in the buffer, we choose quicksort to sort the records
	 *    by specifying the column to be sorted
	 * 3, whether to register the buffer into the blockmanager.
	 * */
    BlockStreamBase* block_for_asking;

    state_.partition_offset_=part_off;

    state_.child_->open(state_.partition_offset_);

	if(sema_open_.try_wait()){
	block_buffer_iterator_=block_buffer_.createIterator();
		open_finished_ = true;
	}
	else{
		while(!open_finished_){
			usleep(1);
		}
	}

    if(createBlockStream(block_for_asking)==false)
    	;
    /* phase 1: store the data in the buffer!
     *          by using multi-threads to speed up
     * */
    unsigned block_offset=0;
    unsigned tuple_count_sum=0;
	BlockStreamBase::BlockStreamTraverseIterator *iterator_for_scan;
    while(state_.child_->next(block_for_asking)){
    	tuple_count_sum+=block_for_asking->getTuplesInBlock();
		block_buffer_.atomicAppendNewBlock(block_for_asking);
		iterator_for_scan=block_buffer_.getBlock(block_offset)->createIterator();
		void *tuple_ptr=0;
		while((tuple_ptr=iterator_for_scan->nextTuple())!=0){
			secondaryArray.push_back(tuple_ptr);
		}
		block_offset++;
		if(createBlockStream(block_for_asking)==false){
			cout<<"error in the create block stream!!!"<<endl;
			return 0;
		}
    }

    /* phase 2: sort the data in the buffer!
     *          by using multi-threads to speed up?
     * TODO: whether to store the sorted data into the blockmanager
     * */
    unsigned long long int time=curtick();
    order(state_.orderbyKey_,tuple_count_sum);
	cout<<"the tuple_count is: "<<tuple_count_sum<<"Total time: "<<getSecond(time)<<" seconds, the swap num is: "<<swap_num<<endl;
    return true;
}

bool BlockStreamSortIterator::next(BlockStreamBase* block){
	/* multi-threads to send the block out*/
	unsigned tuple_size=state_.input_->getTupleMaxSize();
	while(temp_cur<secondaryArray.size()){
		void *desc=0;
		while((desc=block->allocateTuple(tuple_size))){
			memcpy(desc,secondaryArray[temp_cur],tuple_size);
			temp_cur++;
		}
		return true;
	}
    return false;
}

bool BlockStreamSortIterator::close(){
    DynamicBlockBuffer::Iterator it=block_buffer_.createIterator();
    BlockStreamBase* block_to_deallocate;
    while(block_to_deallocate=(BlockStreamBase*)it.nextBlock()){
		block_to_deallocate->~BlockStreamBase();
    }
    state_.child_->close();
    return true;
}


bool BlockStreamSortIterator::createBlockStream(BlockStreamBase*& target)const{
        //TODO: the block allocation should apply for the memory budget from the buffer manager first.
		cout<<"state_.block_size_: "<<state_.block_size_<<endl;
        target=BlockStreamBase::createBlock(state_.input_,state_.block_size_);
        return target!=0;
}
