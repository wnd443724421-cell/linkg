 #ifndef  DEBUG_H
 #define  DEBUG_H
 
 #ifdef __cplusplus
 extern "C" {
 #endif
 //****************C****************************
#define debug_printf_en 


#ifdef debug_printf_en
#define debug_printf( fmt,  arg  ... )  \
	do{printf("[DEBUG] " fmt ,  ##arg );  }while(0);
#define err_printf( fmt,  arg  ... ) \
	do{printf("[%s: %s: line %d]" fmt ,\
		__FILE__, __FUNCTION__, __LINE__,  ##arg );  }while(0);
#else
#define debug_printf( fmt,  arg  ... )
#define err_printf( fmt,  arg  ... )
#endif
 
 //****************C END************************
 #ifdef __cplusplus
 }
 #endif
 
 #endif
