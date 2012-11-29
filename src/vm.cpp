#include <vm.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

TMethod* SmalltalkVM::lookupMethodInCache(TSymbol* selector, TClass* klass)
{
    uint32_t hash = reinterpret_cast<uint32_t>(selector) ^ reinterpret_cast<uint32_t>(klass);
    TMethodCacheEntry& entry = m_lookupCache[hash % LOOKUP_CACHE_SIZE];
    
    if (entry.methodName == selector && entry.receiverClass == klass) {
        m_cacheHits++;
        return entry.method;
    } else {
        m_cacheMisses++;
        return 0;
    }
}

TMethod* SmalltalkVM::lookupMethod(TSymbol* selector, TClass* klass)
{
    // First of all checking the method cache
    // Frequently called methods most likely will be there
    TMethod* result = (TMethod*) lookupMethodInCache(selector, klass);
    if (result)
        return result; // We're lucky!
    
    // Well, maybe we'll be luckier next time. For now we need to do the full search.
    // Scanning through the class hierarchy from the klass up to the Object
    for (TClass* currentClass = klass; currentClass != globals.nilObject; currentClass = currentClass->parentClass) {
        TDictionary* methods = currentClass->methods;
        result = (TMethod*) methods->find(selector);
        if (result)
            return result;
    }
    
    return 0;
}

void SmalltalkVM::flushCache()
{
    for (size_t i = 0; i < LOOKUP_CACHE_SIZE; i++)
        m_lookupCache[i].methodName = 0;
}

#define IP_VALUE (byteCodes[bytePointer] | (byteCodes[bytePointer+1] << 8))

SmalltalkVM::TExecuteResult SmalltalkVM::execute(TProcess* process, uint32_t ticks)
{
    m_rootStack.push_back(process);
    
    // current execution context and the executing method
    TContext* context = process->context;
    TMethod*  method  = context->method;
    
    TByteObject& byteCodes   = *method->byteCodes;
    uint32_t     bytePointer = getIntegerValue(context->bytePointer);
    
    TObjectArray&  stack     = *context->stack;
    uint32_t       stackTop  = getIntegerValue(context->stackTop);

    TObjectArray& temporaries       = *context->temporaries;
    TObjectArray& arguments         = *context->arguments;
    TObjectArray& instanceVariables = *(TObjectArray*) arguments[0];
    TSymbolArray& literals          = *method->literals;
    //initVariablesFromContext(context, *method, byteCodes, bytePointer, stack, stackTop, temporaries, arguments, instanceVariables, literals);
    TObject* returnedValue = globals.nilObject;
    
    while (true) {
        if (ticks && (--ticks == 0)) {
            // Time frame expired
            TProcess* newProcess = (TProcess*) m_rootStack.back(); m_rootStack.pop_back();
            newProcess->context = context;
            newProcess->result = returnedValue;
            context->bytePointer = newInteger(bytePointer);
            context->stackTop = newInteger(stackTop);
            return returnTimeExpired;
        }
            
        // decoding the instruction
        TInstruction instruction;
        instruction.low = (instruction.high = byteCodes[bytePointer++]) & 0x0F;
        instruction.high >>= 4;
        if (instruction.high == extended) {
            instruction.high = instruction.low;
            instruction.low = byteCodes[bytePointer++];
        }
        
        switch (instruction.high) { // 6 pushes, 2 assignes, 1 mark, 3 sendings, 2 do's
            case pushInstance:    stack[stackTop++] = instanceVariables[instruction.low]; break;
            case pushArgument:    stack[stackTop++] = arguments[instruction.low];         break;
            case pushTemporary:   stack[stackTop++] = temporaries[instruction.low];       break;
            case pushLiteral:     stack[stackTop++] = literals[instruction.low];          break;
            case pushConstant: 
                doPushConstant(instruction.low, stack, stackTop); 
                break;
            case pushBlock:
                
                break;
                
            case assignTemporary: temporaries[instruction.low] = stack[stackTop - 1];     break;
            case assignInstance:
                instanceVariables[instruction.low] = stack[stackTop - 1];
                // TODO isDynamicMemory()
                break;
                
            case markArguments: {
                // This operation takes instruction.low arguments 
                // from the top of the stack and creates new array with them
                
                m_rootStack.push_back(context);
                TObjectArray* args = newObject<TObjectArray>(instruction.low);
                
                for (int index = instruction.low - 1; index > 0; index--)
                    (*args)[index] = stack[--stackTop];
                
                stack[stackTop++] = args;
            } break;
                
            case sendMessage: {
                TSymbol* messageSelector = literals[instruction.low];
                TObjectArray* messageArguments = (TObjectArray*) stack[--stackTop];
                doSendMessage(messageSelector, *messageArguments, context, stackTop); 
            } break;
            
            case sendUnary: { // isNil notNil //TODO in the future: catch instruction.low != 0 or 1
                TObject* top = stack[--stackTop];
                bool result = (top == globals.nilObject);
                
                if (instruction.low != 0)
                    result = not result;
                
                returnedValue = result ? globals.trueObject : globals.falseObject;
                stack[stackTop++] = returnedValue;
            } break;
            
            case sendBinary: {
                // Sending a binary operator to an object
                
                // Loading operand objects
                TObject* rightObject = stack[--stackTop];
                TObject* leftObject = stack[--stackTop];
                
                // If operands are both small integers, we need to handle it ourselves
                if (isSmallInteger(leftObject) && isSmallInteger(rightObject)) {
                    // Loading actual operand values
                    uint32_t rightOperand = getIntegerValue(reinterpret_cast<TInteger>(rightObject));
                    uint32_t leftOperand  = getIntegerValue(reinterpret_cast<TInteger>(leftObject));
                    
                    // Performing an operation
                    switch (instruction.low) {
                        case 0: // operator <
                            returnedValue = (leftOperand < rightOperand) ? globals.trueObject : globals.falseObject;
                            break;
                        
                        case 1: // operator <=
                            returnedValue = (leftOperand <= rightOperand) ? globals.trueObject : globals.falseObject;
                            break;
                        
                        case 2: // operator +
                            returnedValue = reinterpret_cast<TObject*>(newInteger(leftOperand+rightOperand)); //FIXME possible overflow?
                            break;
                    }
                    
                    // Pushing result back to the stack
                    stack[stackTop++] = returnedValue;
                } else {
                    // This binary operator is performed on an ordinary object.
                    // We do not know how to handle it, so sending the operation to the receiver
                    
                    TObjectArray* args = newObject<TObjectArray>(2);
                    (*args)[1] = rightObject;
                    (*args)[0] = leftObject;
                    //TODO do the call
                }
            } break;
                
            case doPrimitive: {
                uint8_t primitiveNumber = byteCodes[bytePointer++];
                m_rootStack.push_back(context);
                
                returnedValue = doExecutePrimitive(primitiveNumber, stack, stackTop, *process);
                //if(returnedValue == returnError) //FIXME !!!111
                //    return returnedValue;
                
                context = (TContext*) m_rootStack.back(); m_rootStack.pop_back();
                context = context->previousContext;
                //initVariablesFromContext(context, *method, byteCodes, bytePointer, stack, stackTop, temporaries, arguments, instanceVariables, literals);
                
                stack[stackTop++] = returnedValue;
            } break;
                
            case doSpecial: {
                TExecuteResult result = doDoSpecial(
                    instruction, 
                    context, 
                    stackTop, 
                    method, 
                    bytePointer, 
                    process, 
                    returnedValue);
                
                if (result != returnNoReturn)
                    return result;
            } break;
        }
    }
}


SmalltalkVM::TExecuteResult SmalltalkVM::doDoSpecial(
    TInstruction instruction, 
    TContext* context, 
    uint32_t& stackTop,
    TMethod*& method,
    uint32_t& bytePointer,
    TProcess*& process,
    TObject*& returnedValue)
{
    TByteObject&  byteCodes         = *method->byteCodes;
    TObjectArray& stack             = *context->stack;
    TObjectArray& temporaries       = *context->temporaries;
    TObjectArray& arguments         = *context->arguments;
    TObjectArray& instanceVariables = *(TObjectArray*) arguments[0];
    TSymbolArray& literals          = *method->literals;
    
    switch(instruction.low) {
        case selfReturn: {
            returnedValue = arguments[0]; // FIXME why instanceVariables? bug?
                                          // Have a look at interp.c: 605 and 1434
            context = context->previousContext;
            //initVariablesFromContext(context, *method, byteCodes, bytePointer, stack, stackTop, temporaries, arguments, instanceVariables, literals);
            stack[stackTop++] = returnedValue;
        } break;
        
        case stackReturn:
        {
            returnedValue = stack[--stackTop];
            context = context->previousContext;
            //initVariablesFromContext(context, *method, byteCodes, bytePointer, stack, stackTop, temporaries, arguments, instanceVariables, literals);
            stack[stackTop++] = returnedValue;
            
//             doReturn:
//             context = context->previousContext;
//             goto doReturn2;
//             
//             doReturn2:
//             if(context == 0 || context == globals.nilObject) {
//                 process = (TProcess*) m_rootStack.back(); m_rootStack.pop_back();
//                 process->context = context;
//                 process->result = returnedValue;
//                 return returnReturned;
//             }
//             stack       = *context->stack;
//             stackTop    = getIntegerValue(context->stackTop);
//             stack[stackTop++] = returnedValue;
//             method      = context->method;
//             byteCodes   = *method->byteCodes;
//             bytePointer = getIntegerValue(context->bytePointer);
        } break;
        
        case blockReturn: {
            returnedValue = stack[--stackTop];
            TBlock* contextAsBlock = (TBlock*) context;
            context = contextAsBlock->creatingContext->previousContext;
            //initVariablesFromContext(context, *method, byteCodes, bytePointer, stack, stackTop, temporaries, arguments, instanceVariables, literals);
            stack[stackTop++] = returnedValue;
        } break;
                        
        case duplicate: {
            // Duplicate an object on the stack
            TObject* copy = stack[stackTop - 1];
            stack[stackTop++] = copy;
        } break;
        
        case popTop: stackTop--; break;
        case branch: bytePointer = IP_VALUE; break;
        
        case branchIfTrue: {
            returnedValue = stack[--stackTop];
            
            if(returnedValue == globals.trueObject)
                bytePointer = IP_VALUE;
            else
                bytePointer += 2;
        } break;
        
        case branchIfFalse: {
            returnedValue = stack[--stackTop];
            
            if(returnedValue == globals.falseObject)
                bytePointer = IP_VALUE;
            else
                bytePointer += 2;
        } break;
        
        case sendToSuper: {
            instruction.low = byteCodes[bytePointer++];
            TSymbol* messageSelector = literals[instruction.low];
            TClass*  receiverClass   = instanceVariables.getClass();
            TMethod* method          = lookupMethod(messageSelector, receiverClass);
            //TODO do the call
        } break;
        
        case breakpoint: {
            bytePointer -= 1;
            
            process = (TProcess*) m_rootStack.back(); m_rootStack.pop_back();
            process->context = context;
            process->result = returnedValue;
            context->bytePointer = getIntegerValue(bytePointer);
            context->stackTop = getIntegerValue(stackTop);
            return returnBreak;
        } break;
        
    }
    
    return returnNoReturn;
}

void SmalltalkVM::doPushConstant(uint8_t constant, TObjectArray& stack, uint32_t& stackTop)
{
    switch (constant) {
        case 0: 
        case 1: 
        case 2: 
        case 3: 
        case 4: 
        case 5: 
        case 6: 
        case 7: 
        case 8: 
        case 9: 
            stack[stackTop++] = (TObject*) newInteger(constant);
            break;
            
        case nilConst:   stack[stackTop++] = globals.nilObject;   break;
        case trueConst:  stack[stackTop++] = globals.trueObject;  break;
        case falseConst: stack[stackTop++] = globals.falseObject; break;
        default:
            /* TODO unknown push constant */ ;
            stack[stackTop++] = globals.nilObject;
            
    }
}

void SmalltalkVM::doSendMessage(TSymbol* selector, TObjectArray& arguments, TContext* context, uint32_t& stackTop)
{
    TClass*  receiverClass    = (TClass*) arguments[0];
    
    TMethod* method = lookupMethod(selector, receiverClass);
    if (! method) {
        // Oops. Nothing was found.
        // Seems that current object does not understand this message
        
        if (selector == globals.badMethodSymbol) {
            // Something really bad happened
            // TODO error
            exit(1);
        }
    }
    
}

TObject* SmalltalkVM::newObject(TSymbol* className, size_t objectSize)
{
    // TODO fast access to common classes
    TClass* klass = (TClass*) m_image->getGlobal(className);
    if (!klass)
        return globals.nilObject;
    
    // Slot size is computed depending on the object type
    size_t slotSize = 0;
//     if (T::InstancesAreBinary())    
//         slotSize = sizeof(T) + objectSize;
//     else 
        slotSize = sizeof(TObject) + objectSize * sizeof(TObject*);
    
    void* objectSlot = malloc(slotSize); // TODO llvm_gc_allocate
    if (!objectSlot)
        return globals.nilObject;
    
    TObject* instance = new (objectSlot) TObject(objectSize, klass);
    for (uint32_t i = 0; i < objectSize; i++)
        instance->putField(i, globals.nilObject);
    
    return instance;
}

TObject* SmalltalkVM::newObject(TClass* klass)
{
    uint32_t fieldsCount = getIntegerValue(klass->instanceSize);
    uint32_t slotSize = sizeof(TObject) + fieldsCount * sizeof(TObject*);
    
    void* objectSlot = malloc(slotSize); // TODO llvm_gc_allocate
    if (!objectSlot)
        return globals.nilObject;
    
    TObject* instance = new (objectSlot) TObject(slotSize, klass);
    for (uint32_t i = 0; i < fieldsCount; i++)
        instance->putField(i, globals.nilObject);
    
    return instance;
}

TObject* SmalltalkVM::doExecutePrimitive(uint8_t opcode, TObjectArray& stack, uint32_t& stackTop, TProcess& process)
{
    switch(opcode)
    {
        case 1: // operator ==
        {
            TObject* arg2   = stack[--stackTop];
            TObject* arg1   = stack[--stackTop];
            
            if(arg1 == arg2)
                return globals.trueObject;
            else
                return globals.falseObject;
        } break;
        
        case 2: // return class
        {
            TObject* object = stack[--stackTop];
            return isSmallInteger(object) ? globals.smallIntClass : object->getClass();
        } break;
        
        case 3: // put char to stdout
        {
            TInteger charObject = reinterpret_cast<TInteger>(stack[--stackTop]);
            uint8_t  charValue = getIntegerValue(charObject);
            putchar(charValue);
            return globals.nilObject;
        } break;
        
        case 4: // return size of object
        {
            TObject* object = stack[--stackTop];
            uint32_t returnedSize = isSmallInteger(object) ? 0 : object->getSize();
            return reinterpret_cast<TObject*>(newInteger(returnedSize));
        } break;
        
        case 6: // start new process
        {
            TInteger value = reinterpret_cast<TInteger>(stack[--stackTop]);
            uint32_t ticks = getIntegerValue(value);
            TProcess* newProcess = (TProcess*) stack[--stackTop];
            
            // FIXME possible stack overflow due to recursive call
            int result = this->execute(newProcess, ticks);
            return reinterpret_cast<TObject*>(newInteger(result));
        } break;
        
        case 7: // allocate new object
        {
            TObject* size  = stack[--stackTop];
            TClass*  klass = (TClass*) stack[--stackTop];
            uint32_t fieldsCount = getIntegerValue(reinterpret_cast<TInteger>(size));
            return newObject(klass->name, fieldsCount);
        } break;
        
        case 8:
        {
            //TODO
        } break;
        
        case 9:
        {
            int32_t input = getchar();
            if(input == EOF)
                return globals.nilObject;
            else
                return reinterpret_cast<TObject*>(newInteger(input));
        } break;
        
        case 10: // small int +
        case 11: // small int /
        case 12: // small int %
        case 13: // small int <
        case 14: // small int ==
        case 15: // small int *
        case 16: // small int -
        case 36: // bit or
        case 37: // bit and
        case 39: // bit shift
        {
            // Loading operand objects
            TObject* rightObject = stack[--stackTop];
            TObject* leftObject  = stack[--stackTop];
            if ( !isSmallInteger(leftObject) || !isSmallInteger(rightObject) ) {
                failPrimitive(stack, stackTop);
                break;
            }
                
            // Extracting values
            uint32_t leftOperand  = getIntegerValue(reinterpret_cast<TInteger>(leftObject));
            uint32_t rightOperand = getIntegerValue(reinterpret_cast<TInteger>(rightObject));
            
            // Performing an operation
            TObject* result = doSmallInt(opcode, leftOperand, rightOperand);
            if (result == globals.nilObject) {
                failPrimitive(stack, stackTop);
                break;
            }
            return result;
        } break;
        
        case 18: // turn on debugging
        {
            //TODO
        } break;
        
        case 19: // error
        {
            m_rootStack.pop_back();
            TContext* context = process.context;
            process = *(TProcess*) m_rootStack.back(); m_rootStack.pop_back();
            process.context = context;
            //return returnError; TODO cast 
        } break;
        
        case 20: // ByteArray alloc
        {
            uint32_t objectSize = getIntegerValue(reinterpret_cast<TInteger>(stack[--stackTop]));
            TClass*  klass = (TClass*) stack[--stackTop];
            
            size_t slotSize  = sizeof(TByteArray) + objectSize * sizeof(TByteArray*);
            void* objectSlot = m_memoryManager->allocate(slotSize);
            if (!objectSlot)
                return globals.nilObject;
            
            TByteArray* instance = (TByteArray*) new (objectSlot) TByteObject(objectSize, klass);
            return (TObject*) instance;
        } break;
        
        case 5:  // Array:at:put
        case 24: // Array:at
        {
            TObject* indexObject = stack[--stackTop];
            TObjectArray* array  = (TObjectArray*) stack[--stackTop];
            TObject* valueObject;
            
            // If the method is Array:at:put then pop a value from the stack
            if (opcode == 5) 
                valueObject = stack[--stackTop];
            
            if (! isSmallInteger(indexObject) ) {
                failPrimitive(stack, stackTop);
                break;
            }

            // Smalltalk indexes arrays starting from 1, not from 0
            // So we need to recalculate the actual array index before
            uint32_t actualIndex = getIntegerValue(reinterpret_cast<TInteger>(indexObject)) - 1; 
            
            // Checking boundaries
            if (actualIndex >= array->getSize()) {
                failPrimitive(stack, stackTop);
                break;
            }
            
            if(opcode == 24) 
                // Array:at
                return array->getField(actualIndex);
            else { 
                // Array:at:put
                array->putField(actualIndex, valueObject);
                
                // TODO gc ?
                
                // Return self
                return (TObject*) array;
            }
        } break;
        
        case 21: // String:at
        case 22: // String:at:put
        {
            TObject* indexObject        = stack[--stackTop];
            TString* string             = (TString*) stack[--stackTop];
            TObject* valueObject;
            
            // If the method is String:at:put then pop a value from the stack
            if (opcode == 22) 
                valueObject = stack[--stackTop];
            
            if ( !isSmallInteger(indexObject) ) {
                failPrimitive(stack, stackTop);
                break;
            }
            
            // Smalltalk indexes arrays starting from 1, not from 0
            // So we need to recalculate the actual array index before
            uint32_t index = getIntegerValue(reinterpret_cast<TInteger>(indexObject)) - 1;
            if (index >= string->getSize()) {
                failPrimitive(stack, stackTop);
                break;
            }
            
            if(opcode == 21) 
                // String:at
                return reinterpret_cast<TObject*>(newInteger( string->getByte(index) ));
            else { 
                // String:at:put
                TInteger value = reinterpret_cast<TInteger>(valueObject);
                string->putByte(index, getIntegerValue(value));
                return (TObject*) string;
            }
        } break;
        
        case 23: // ByteObject clone
        {
            TClass* klass = (TClass*) stack[--stackTop];
            TByteObject* original = (TByteObject*) stack[--stackTop];
            
            // Creating clone
            uint32_t dataSize  = original->getSize();
            TByteObject* clone = (TByteObject*) newObject(klass->name, dataSize); // FIXME direct method
            clone->setClass(klass);
            
            // Cloning data
            for (uint32_t i = 0; i < dataSize; i++)
                (*clone)[i] = (*original)[i];
            
            return (TObject*) clone;
        } break;
        
        case 25: // Integer /
        case 26: // Integer %
        case 27: // Integer +
        case 28: // Integer *
        case 29: // Integer -
        case 30: // Integer <
        case 31: // Integer ==
        {
            //TODO
        } break;
        
        case 32: // Integer new
        {
            TObject* object = stack[--stackTop];
            if (! isSmallInteger(object)) {
                failPrimitive(stack, stackTop);
                break;
            }
            
            TInteger integer = reinterpret_cast<TInteger>(object);
            uint32_t value = getIntegerValue(integer);
            
            return reinterpret_cast<TObject*>(newInteger(value));
        } break;
        
        case 33:
        {
            
        } break;
        
        case 34:
        {
            flushCache();
            //FIXME returnedValue is not to be globals.nilObject
        } break;
        
        case 35:
        {
            
        } break;
        
        case 38:
        {
            
        } break;
        
        case 40:
        {
            
        } break;
        
        default:
        {
            
        } break;
    }
    
    return globals.nilObject;
}

template<> TObjectArray* SmalltalkVM::newObject<TObjectArray>(size_t objectSize /*= 0*/)
{
    TClass* klass = globals.arrayClass;
    
    // Slot size is computed depending on the object type
    size_t slotSize = sizeof(TObjectArray) + objectSize * sizeof(TObjectArray*);
    
    void* objectSlot = m_memoryManager->allocate(slotSize);
    if (!objectSlot)
        return (TObjectArray*) globals.nilObject;
    
    TObjectArray* instance = (TObjectArray*) new (objectSlot) TObject(objectSize, klass);
    for (uint32_t i = 0; i < objectSize; i++)
        instance->putField(i, globals.nilObject);
    
    return instance;
}

TObject* SmalltalkVM::doSmallInt( uint32_t opcode, uint32_t leftOperand, uint32_t rightOperand)
{
    switch(opcode) {
        case 10: // operator +
            return reinterpret_cast<TObject*>(newInteger( leftOperand + rightOperand )); //FIXME possible overflow
        
        case 11: // operator /
            if (rightOperand == 0)
                return globals.nilObject;
            return reinterpret_cast<TObject*>(newInteger( leftOperand / rightOperand ));
        
        case 12: // operator %
            if (rightOperand == 0)
                return globals.nilObject;
            return reinterpret_cast<TObject*>(newInteger( leftOperand % rightOperand ));
        
        case 13: // operator <
            if (leftOperand < rightOperand)
                return globals.trueObject;
            else
                return globals.falseObject;
        
        case 14: // operator ==
            if (leftOperand == rightOperand)
                return globals.trueObject;
            else
                return globals.falseObject;
        
        case 15: // operator *
            return reinterpret_cast<TObject*>(newInteger( leftOperand * rightOperand )); //FIXME possible overflow
        
        case 16: // operator -
            return reinterpret_cast<TObject*>(newInteger( leftOperand - rightOperand )); //FIXME possible overflow
        
        case 36: // operator |
            return reinterpret_cast<TObject*>(newInteger( leftOperand | rightOperand ));
        
        case 37: // operator &
            return reinterpret_cast<TObject*>(newInteger( leftOperand & rightOperand ));
        
        case 39: { 
            // operator << if rightOperand < 0, operator >> if rightOperand >= 0
            
            uint32_t result = 0;
            int32_t  signedRightOperand = (int32_t) rightOperand;
            
            if (signedRightOperand < 0) {
                //shift right 
                result = leftOperand >> -signedRightOperand;
            } else {
                // shift left ; catch overflow 
                result = leftOperand << rightOperand;
                if (leftOperand > result) {
                    return globals.nilObject;
                }
            }
            
            return reinterpret_cast<TObject*>(newInteger( result ));
        }
        
        default: 
            return globals.nilObject; /* FIXME possible error */
    }
}

void SmalltalkVM::failPrimitive(TObjectArray& stack, uint32_t& stackTop) {
    stack[stackTop++] = globals.nilObject;
}

void SmalltalkVM::initVariablesFromContext(TContext* context,
                                    TMethod& method,
                                    TByteObject& byteCodes,
                                    uint32_t& bytePointer,
                                    TObjectArray& stack,
                                    uint32_t& stackTop,
                                    TObjectArray& temporaries,
                                    TObjectArray& arguments,
                                    TObjectArray& instanceVariables,
                                    TSymbolArray& literals)
{
    method            = *context->method;
    byteCodes         = *method.byteCodes;
    bytePointer       = getIntegerValue(context->bytePointer);
    stack             = *context->stack;
    stackTop          = getIntegerValue(context->stackTop);
    
    temporaries       = *context->temporaries;
    arguments         = *context->arguments;
    instanceVariables = *(TObjectArray*) arguments[0];
    literals          = *method.literals;
}