/*
#  Created by Boyd Multerer on June 6, 2018.
#  Copyright © 2018 Kry10 Industries. All rights reserved.
#

Functions to load textures onto the graphics card
*/


int get_tx_id(void* p_tx_ids, char* p_key);

void receive_put_tx_blob( int* p_msg_length, driver_data_t* window );
void receive_put_tx_pixels(int* p_msg_length, driver_data_t* window);
void receive_free_tx_id( int* p_msg_length, driver_data_t* window );
