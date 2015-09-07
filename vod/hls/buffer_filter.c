#include "buffer_filter.h"

enum {
	STATE_INITIAL,
	STATE_FRAME_STARTED,
	STATE_FRAME_FLUSHED,
	STATE_DIRECT,
};

vod_status_t 
buffer_filter_init(
	buffer_filter_t* state, 
	request_context_t* request_context,
	const media_filter_t* next_filter,
	void* next_filter_context,
	uint32_t size)
{
	state->request_context = request_context;
	state->next_filter = next_filter;
	state->next_filter_context = next_filter_context;
	state->size = size;

	state->cur_state = STATE_INITIAL;

	state->used_size = 0;
	state->last_flush_size = 0;

	if (request_context->simulation_only)
	{
		return VOD_OK;
	}

	state->start_pos = vod_alloc(request_context->pool, size);
	if (state->start_pos == NULL)
	{
		vod_log_debug0(VOD_LOG_DEBUG_LEVEL, request_context->log, 0,
			"buffer_filter_init: vod_alloc failed");
		return VOD_ALLOC_FAILED;
	}
	state->end_pos = state->start_pos + size;
	state->cur_pos = state->start_pos;
	state->last_flush_pos = state->cur_pos;
	
	return VOD_OK;
}

static vod_status_t 
buffer_filter_start_frame(void* context, output_frame_t* frame)
{
	buffer_filter_t* state = (buffer_filter_t*)context;
	
	switch (state->cur_state)
	{
	case STATE_INITIAL:
		state->cur_frame = *frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		break;
		
	default:
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"buffer_filter_start_frame: invalid state %d", state->cur_state);
		return VOD_UNEXPECTED;
	}

	state->last_frame = *frame;
	state->cur_state = STATE_FRAME_STARTED;
	
	return VOD_OK;
}

vod_status_t 
buffer_filter_force_flush(buffer_filter_t* state, bool_t last_stream_frame)
{
	vod_status_t rc;

	// if nothing was written since the last frame flush, nothing to do
	if (state->last_flush_pos <= state->start_pos)
	{
		return VOD_OK;
	}
	
	// Note: at this point state can only be either STATE_FRAME_STARTED or STATE_FRAME_FLUSHED
		
	// write all buffered data up to the last frame flush position
	rc = state->next_filter->start_frame(state->next_filter_context, &state->cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	rc = state->next_filter->write(state->next_filter_context, state->start_pos, state->last_flush_pos - state->start_pos);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	rc = state->next_filter->flush_frame(state->next_filter_context, last_stream_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	// move back any remaining data
	vod_memmove(state->start_pos, state->last_flush_pos, state->cur_pos - state->last_flush_pos);
	state->cur_pos -= (state->last_flush_pos - state->start_pos);
	state->last_flush_pos = state->start_pos;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		state->cur_frame = state->last_frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		state->cur_state = STATE_INITIAL;
		break;
	}
	
	return VOD_OK;
}

static vod_status_t 
buffer_filter_write(void* context, const u_char* buffer, uint32_t size)
{
	buffer_filter_t* state = (buffer_filter_t*)context;
	vod_status_t rc;

	switch (state->cur_state)
	{
	case STATE_DIRECT:
		// in direct mode just pass the write to the next filter
		return state->next_filter->write(state->next_filter_context, buffer, size);
		
	case STATE_FRAME_STARTED:
		break;				// handled outside the switch
		
	default:
		vod_log_error(VOD_LOG_ERR, state->request_context->log, 0,
			"buffer_filter_write: invalid state %d", state->cur_state);
		return VOD_UNEXPECTED;		// unexpected
	}
	
	// if there is not enough room try flushing the buffer
	if (state->cur_pos + size > state->end_pos)
	{
		rc = buffer_filter_force_flush(state, FALSE);
		if (rc != VOD_OK)
		{
			return rc;
		}
	}
	
	// if there is enough room in the buffer, copy the input data
	if (state->cur_pos + size <= state->end_pos)
	{
		state->cur_pos = vod_copy(state->cur_pos, buffer, size);
		return VOD_OK;
	}
	
	// still not enough room after flushing - write directly to the next filter
	state->cur_state = STATE_DIRECT;
	
	rc = state->next_filter->start_frame(state->next_filter_context, &state->cur_frame);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	if (state->cur_pos > state->start_pos)
	{
		rc = state->next_filter->write(state->next_filter_context, state->start_pos, state->cur_pos - state->start_pos);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_pos = state->start_pos;
	}
	
	rc = state->next_filter->write(state->next_filter_context, buffer, size);
	if (rc != VOD_OK)
	{
		return rc;
	}
	
	return VOD_OK;
}

static vod_status_t 
buffer_filter_flush_frame(void* context, bool_t last_stream_frame)
{
	buffer_filter_t* state = (buffer_filter_t*)context;
	vod_status_t rc;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		// update the last flush position
		state->last_flush_pos = state->cur_pos;
		state->cur_state = STATE_FRAME_FLUSHED;

		if (last_stream_frame)
		{
			rc = buffer_filter_force_flush(state, TRUE);
			if (rc != VOD_OK)
			{
				return rc;
			}
		}
		break;
	
	case STATE_DIRECT:
		// pass the frame flush to the next filter
		rc = state->next_filter->flush_frame(state->next_filter_context, last_stream_frame);
		if (rc != VOD_OK)
		{
			return rc;
		}
		state->cur_state = STATE_INITIAL;
		break;
		
		// Note: nothing to do for the other states
	}
	
	return VOD_OK;
}

bool_t 
buffer_filter_get_dts(buffer_filter_t* state, uint64_t* dts)
{
	if (state->cur_state == STATE_INITIAL)
	{
		return FALSE;
	}
	
	*dts = state->cur_frame.dts;
	return TRUE;
}


void
buffer_filter_simulated_force_flush(buffer_filter_t* state, bool_t last_stream_frame)
{
	if (state->last_flush_size <= 0)
	{
		return;
	}
	
	state->next_filter->simulated_start_frame(state->next_filter_context, &state->cur_frame);	
	state->next_filter->simulated_write(state->next_filter_context, state->last_flush_size);
	state->next_filter->simulated_flush_frame(state->next_filter_context, last_stream_frame);
	
	state->used_size -= state->last_flush_size;
	state->last_flush_size = 0;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		state->cur_frame = state->last_frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		state->cur_state = STATE_INITIAL;
		break;
	}
}

static void 
buffer_filter_simulated_start_frame(void* context, output_frame_t* frame)
{
	buffer_filter_t* state = (buffer_filter_t*)context;
	
	switch (state->cur_state)
	{
	case STATE_INITIAL:
		state->cur_frame = *frame;
		break;
		
	case STATE_FRAME_FLUSHED:
		break;
	}

	state->last_frame = *frame;
	state->cur_state = STATE_FRAME_STARTED;
}

static void 
buffer_filter_simulated_write(void* context, uint32_t size)
{
	buffer_filter_t* state = (buffer_filter_t*)context;

	switch (state->cur_state)
	{
	case STATE_DIRECT:
		state->next_filter->simulated_write(state->next_filter_context, size);
		return;
		
	case STATE_FRAME_STARTED:
		break;				// handled outside the switch
	}
	
	if (state->used_size + size > state->size)
	{
		buffer_filter_simulated_force_flush(state, FALSE);
	}
	
	if (state->used_size + size <= state->size)
	{
		state->used_size += size;
		return;
	}
	
	state->cur_state = STATE_DIRECT;
	
	state->next_filter->simulated_start_frame(state->next_filter_context, &state->cur_frame);
	
	state->next_filter->simulated_write(state->next_filter_context, state->used_size + size);

	state->used_size = 0;
}

static void 
buffer_filter_simulated_flush_frame(void* context, bool_t last_stream_frame)
{
	buffer_filter_t* state = (buffer_filter_t*)context;

	switch (state->cur_state)
	{
	case STATE_FRAME_STARTED:
		// update the last flush position
		state->last_flush_size = state->used_size;
		state->cur_state = STATE_FRAME_FLUSHED;

		if (last_stream_frame)
		{
			buffer_filter_simulated_force_flush(state, TRUE);
		}
		break;
	
	case STATE_DIRECT:
		// pass the frame flush to the next filter
		state->next_filter->simulated_flush_frame(state->next_filter_context, last_stream_frame);
		state->cur_state = STATE_INITIAL;
		break;
	}
}

const media_filter_t buffer_filter = {
	buffer_filter_start_frame,
	buffer_filter_write,
	buffer_filter_flush_frame,
	buffer_filter_simulated_start_frame,
	buffer_filter_simulated_write,
	buffer_filter_simulated_flush_frame,
};
