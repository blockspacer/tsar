int main() {
  int I;
  I = 0;
#pragma sapfor analysis loop(implicit)
#pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
head:
  if (I < 10)
    goto end;
  I = I + 1;
  goto head;
end:
  return 0;
}